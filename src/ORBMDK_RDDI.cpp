/**
 * @file ORBMDK_RDDI.cpp
 * @brief RDDI (Remote Debug Driver Interface) 实现
 *
 * 提供符合 Keil uVision RDDI 规范的接口，用于 AGDI 层调用
 * 兼容 elaphureLinkAGDI
 */

#include "pch.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_HID.h"
#include "ORBMDK_USB_Bulk.h"
#include "ORBMDK_ITM_Decoder.h"
#include "ORBMDK_Trace.h"
#include "ORBMDK_Log.h"

using namespace ORBMDK;

#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>

// ============================================================================
// 日志
//
// ★ 2026-09-30：改为统一实现（src/ORBMDK_Log.cpp，COMPAT_ANALYSIS §8.3）。
//   —— 它们与 ORBMDK_HID.cpp 的 HID_Log 是两套逻辑重复的实现（§8.2）。
//
// 排障开关（对所有模块一致，免重启、免命令行）：
//   环境变量 ORBMDK_LOG_LEVEL / ORBMDK_LOG_FILE（进程启动读一次，文件优先）
// ============================================================================
#define ORBMDK_LOG_MODULE "RDDI"



// 日志宏：既有调用点（LOG_DEBUG/INFO/WARN/ERROR，全项目数百处）保持不变，
// 只是转发到统一实现（src/ORBMDK_Log.cpp）。模块名 "RDDI" 由 ORBMDK_LOG_MODULE 提供。
//
// 旧的"模块级宏"（LOG_HID_* / LOG_BULK_* / LOG_TRACE_*）是死代码 ——
// 它们写在本 .cpp 里，别的编译单元看不到（§8.2(2)），已按 §8.3 迁移第 4 步删除。
#define LOG_DEBUG(...) ORBMDK_LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)  ORBMDK_LOG_INFO(__VA_ARGS__)
#define LOG_WARN(...)  ORBMDK_LOG_WARN(__VA_ARGS__)
#define LOG_ERROR(...) ORBMDK_LOG_ERROR(__VA_ARGS__)

// ============================================================================
// Constants
// ============================================================================

// DAP Register offset mapping
// For DP registers: 0x00, 0x04, 0x08, 0x0C
// For AP registers: 0x01, 0x05, 0x09, 0x0D (with APnDP bit set)
static const uint8_t kDapRegOffsetMap[8] = {
    0x00, 0x04, 0x08, 0x0C,  // DP registers
    0x01, 0x05, 0x09, 0x0D,  // AP registers (with APnDP bit)
};

// Default debug configuration
static constexpr uint32_t kDefaultClock = 1000000;  // 1 MHz
static constexpr uint8_t kDefaultWaitRetry = 100;
static constexpr uint8_t kDefaultMatchRetry = 10;
// JTAG 链路自愈的连续失败上限（超过则停止重初始化，避免刷日志/占满探针）
static constexpr int kJtagRecoverMaxFails = 3;

// ---------------------------------------------------------------------------
// 上报给宿主的版本串（Identify(idNo=4)）—— **不是**这里定义的一个固定常量
//
// ⚠ 语义（CMSIS-DAP v2.1.2）：这里回的是 **CMSIS-DAP 协议版本**（"2.1.0" 形态），
//   不是产品固件版本（那是 `DAP_Info(0x09)`，多数固件不给）。AGDI 的 `cmp eax, 2`
//   门控要的正是协议版本 —— 见下方"机理"。
//
// 取值来自**设备自报的 CMSIS-DAP `DAP_Info(0x04)`**（走已连上的那条传输，
// 由 ORBMDK::DAP_GetInfo 内部按当前模式分发到 HID 或 Bulk），在 RDDI_Open 里
// 问一次并缓存进 ctx（见那里的说明）。
//
// 机理（§17.6 反汇编；§17.5 单变量对照实验）：
// AGDI 把该串按 "%lu.%lu.%lu" 解析并取**主版本号**，`cmp eax, 2` 决定是否切到它
// 自己的"多 DAP 设备"分支；那次观测里该分支走不通 —— **跳过全部设备枚举**
// （日志里没有 GetNumberOfDevices / GetDeviceIDList / ConfigureDebugger），
// Connect 成功之后立刻 Disconnect/Close，界面报 RDDI-DAP Error。
// 当时的实测（A/B 单变量，唯一变化就是这个串）：串 = 设备自报原样 "2.1.0"（主版本 2）
// → 失败；串 = "1.0.0"（主版本 1）→ 完整枚举 + 正常调试下载。V2 与 HID 一致。
// **注意**：那次观测发生在流式 sink 那 13 个接口实现之前，失败可能正源于接口缺失
// （AGDI 来注册 sink 时无接口可调），不能直接推广到当前代码。
//
// 当前口径（2026-10-01 放开，见 Todo.md.bak §18.10-C）：**设备的真实版本照问、照写日志**
// （RDDI_Open 里的 LOG_INFO 原样打印设备回串），但上报给主机的串经
// ORBMDK_NormalizeProtocolVersion 把主版本提升到 >= 2，用于打开 AGDI 的流式分支。
// 若实机回归出现上面那种"连上即断 / RDDI-DAP Error"，即为回退判据，
// 回退步骤见 Todo.md.bak §18.10-C「判死条件与回退」（口径说明在 include/ORBMDK.h）。
//
// 下面这个常量只在**问不到设备**时使用（兜底）。⚠️ 它取 "1.0.0"（主版本 1），与上面
// "归一化把主版本抬到 >= 2"**故意不一致**：兜底意味着设备状态未知（Identify case 4 的
// 幽灵 v1 分支也复用它），此时保持 streaming 门控**关闭**才是保守侧 —— 若这里也抬到
// >= 2，就等于在探针可能根本没答话的情况下宣称流式能力。
// （旧注释写"取值与放开后的口径一致"，是错的，2026-10-03 纠正。）
// ---------------------------------------------------------------------------
static constexpr const char* kFallbackProtocolVersion = ORBMDK_FALLBACK_VERSION_STRING;

// ----------------------------------------------------------------------------
// 单 DAP 枚举常量
//
// ARM rddi_dap.h 规定的调用顺序为：
//     RDDI_Open → DAP_Configure → DAP_GetNumberOfDAPs → DAP_GetDAPIDList
//               → DAP_Connect → ...
// 即 DAP_GetNumberOfDAPs / DAP_GetDAPIDList 会在 **连接目标之前** 被调用，
// 且规范明确要求这两个函数 "does not communicate with the target"。
//
// 因此它们不能依赖需要连接目标才能得到的 IDCODE 列表（dapIdList）。
// ORBTrace 是单 DAP 设备，RDDI 层的 DAP ID 恒为 0。
//
// 历史问题：DAP_GetNumberOfDAPs 曾返回 dapIdList.size()，而该列表只有
// CMSIS_DAP_DetectNumberOfDAPs（需连接目标）才会填充，导致 Keil 在 Connect
// 之前拿到 0 个 DAP，报 "No Debug Unit Found"。
// ----------------------------------------------------------------------------
static constexpr int kSingleDapCount = 1;
static constexpr int kSingleDapId    = 0;

// 单次块传输（ID_DAP_TRANSFER_BLOCK）能携带的最大字数。
// HID 报告负载 64 字节，DAP_TransferBlock 命令头 5 字节：
//     (64 - 5) / 4 = 14
// 上限同时受 ORBMDK_HID_DAPCommand 的长度校验约束（见 ORBMDK_HID.cpp）。
static constexpr int kMaxBlockWords = 14;

// 多笔传输（D2）一个批量最多几笔的**栈缓冲硬上限**。实际值取
// min(kMaxMultiBatch, ORBMDK::DAP_TransferMultiMax()) —— 后者按当前出包长度算
// （V1/HID 12 笔，V2/511 B 101 笔）。
static constexpr int kMaxMultiBatch = 100;

// V2 Bulk 下单次块传输的字数**硬上限**（防呆天花板，不随标定值/描述符放大）。
// 126 来自**固件**（orbtrace/debug/cmsis_dap.py）：响应 RAM 深度 127 字（地址 0..126）、
// txedLen 9 bit（max 511）—— 127 字时 adr*4+4=512 溢出且越 RAM 末字，126 字=508 顶格。
// ★ 不得按主机侧 1024 缓冲放大（原值 250 即此误）；固件对字数不夹取，越界会被 B1 升级成"永久变哑"。
// 实际字数由 BlockWordsLimit() 按出包长度决定（orbtrace 标定 508 -> 125 字），不受此处影响。
static constexpr int kMaxBlockWordsHardLimit = 126;

// 编译期守门：读方向响应长度 (words*4+4) 必须在固件包长/位宽之内。
static_assert(kMaxBlockWordsHardLimit * 4 + 4 <= 508,
              "块传输字数上限超出固件响应包长/位宽（见 bug.md 附录 A5）");

// ---------------------------------------------------------------------------
// 固件 JTAG 链容量硬上限：jtagIF.v 的链参数是**硬编码位宽** ——
//   irlenx = 6 x 5 bit（≤ 6 个器件、单器件 IR ≤ 31）、dev / ndevs 各 3 bit。
// 装不下的布局**不下发** ID_DAP_JTAG_CONFIGURE：硬发会被固件静默截断
// （IR 按 5 bit 截断 / 器件数溢出），走链错 → 通道静默变哑。
// 本层 JTAG 引擎走 ID_DAP_JTAG_SEQUENCE 通用位流，不依赖固件那份链布局，
// 所以"不下发"只丢一次同步、不影响功能。
// ---------------------------------------------------------------------------
static constexpr int kJtagFwMaxDevices = 6;
static constexpr int kJtagFwMaxIrBits  = 31;

/// 链布局能否装进固件的 jtagIF 寄存器（false = 不要下发）。
static bool JtagChainFitsFirmware(int count, const uint8_t* irLens)
{
    if (count <= 0 || count > kJtagFwMaxDevices) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        const int len = irLens ? irLens[i] : 0;
        if (len <= 0 || len > kJtagFwMaxIrBits) {
            return false;   // 长度 0 无效；> 31 会被 5 bit 静默截断
        }
    }
    return true;
}

// 块传输，**仅 SWD 启用**（JTAG 恒不走该命令，见 EnsureBlockTransferProbed），
// 实际是否使用由运行时自动探测决定，没有可绕过的开关。
//
// 背景：DAP_RegWriteRepeat / DAP_RegReadRepeat 原先逐字调用 DAP_Transfer，
// 每字一次 USB 往返。改用 ID_DAP_TRANSFER_BLOCK 后单次往返可带
// kMaxBlockWords(=14) 个字，约 14×（见本文第九节）。
//
// 但并非所有 CMSIS-DAP 固件都实现了 ID_DAP_TRANSFER_BLOCK。因此本层做
// **能力自动探测 + 不兼容回退**：
//   - 每个上下文首次用到块传输时，先用一次只读探测（读 DP IDCODE）确认；
//   - 不支持则永久回退到逐字 DAP_Transfer，功能不受影响，只是慢；
//   - 传输过程中块传输若意外失败，也会立即回退（双保险）。

// ---------------------------------------------------------------------------
// 单次块传输能携带的字数（运行时决定，烧录吞吐的主杠杆）
//
//   V2 Bulk：一条命令必须塞进"一个出包"里才会被固件立即派发（见
//            _bulkPacketSize / _calibrateOutPacket）。出包长度由开机自标定得到，
//            通常是 64（FS）或 508 左右（HS）。可用字数 = (出包 - 5) / 4，
//            HS 下约 125 字，是 V1 上限（14）的近 9 倍。
//   V1 HID：报告负载固定 64 字节，(64 - 5) / 4 = 14，与历史一致。
//
// 出包长度 ≤ 64 时本函数自然退回 14，无需额外分支。
// ---------------------------------------------------------------------------
static int BlockWordsLimit()
{
    const int pkt = ORBMDK_USB_Bulk_GetMaxCommandBytes();   // 0 = HID / 未连接
    if (pkt > 0) {
        int words = (pkt - 5) / 4;                          // 5 = DAP_TransferBlock 命令头
        if (words > kMaxBlockWordsHardLimit) {
            words = kMaxBlockWordsHardLimit;
        }
        if (words < 1) {
            words = 1;
        }
        return words;
    }
    return kMaxBlockWords;
}

// ============================================================================
// Context - 使用 map 管理，避免 vector 扩容导致指针失效
// ============================================================================

struct RDDIContext {
    bool initialized = false;
    int handle = -1;  // Handle 索引

    // 块传输可用性（**仅 SWD 使用**，JTAG 恒为 false）。默认乐观开启，
    // 首次用到时由 EnsureBlockTransferProbed() 探测一次并落定；探测或后续
    // 传输失败都会置 false，之后永久回退到逐字 DAP_Transfer。
    bool blockTransferSupported = true;
    bool blockTransferProbed    = false;

    // 多笔传输（D2，**仅 SWD**）：把连续的多笔 DAP_Transfer 合成一条命令。
    // 默认乐观开启，首次用到 DAP_RegAccessBlock 时探测一次；探测或后续失败即置 false，
    // 本会话永久回退到逐笔（与块传输同一套"先探测、失败即回退"的做法）。
    bool transferMultiSupported = true;
    bool transferMultiProbed    = false;

    // 等待值匹配的加速开关（D1）：
    //   true  = 主机侧先读一次；若已满足就走（1 往返，与历史一致）；**未满足时**改让
    //           固件用 MATCH_VALUE 在其内部按 match_retry 等（1 往返）+ 补一次普通读取值，
    //           把最坏 100 次往返压到 3 次；
    //   false = 回到纯主机侧轮询（固件忽略 match 位 / 失配时自动置 false）。
    // ⚠ 不能把匹配读放进合并批量：该固件的匹配读**不回数据**（见 ProbeTransferMulti 说明）。
    bool waitMatchSupported = true;

    // Debug configuration
    bool isSWD = true;
    // JTAG（§18.9 落地）：链上器件的 IR 长度与个数。
    // 单器件 CoreSight JTAG-DP（Cortex-M）的 IR = 4 位；多 TAP 链暂不支持。
    //
    // ⚠️ jtagDevCount 初值必须是 **0**：它会被 CMSIS_DAP_JTAG_GetIRLengths 直接
    // 报给宿主。SWD 会话里报"有 1 个 JTAG 器件"会让宿主认为链上存在 JTAG 设备 ——
    // 这是纯 SWD 场景下不该出现的信息。只有 JTAG 建链/DAP_JTAG_Configure 成功才置非 0。
    uint8_t jtagIrLength = 4;
    int     jtagDevCount = 0;

    // JTAG 引擎的链布局（§18.9）：探针只提供"通用位流"（ID_DAP_JTAG_SEQUENCE），
    // 选 IR / 组 35 位 DR / 处理 posted read 全部由本层实现（见 JtagDapTransfer）。
    uint8_t jtagIrLens[8] = { 4, 0, 0, 0, 0, 0, 0, 0 }; // 器件 i 的 IR 位数，i=0 离 TDO 最近
    int     jtagChainCount  = 0;                        // 链上器件数（扫链得出）
    int     jtagDpIndex     = 0;                        // JTAG-DP 在链上的位置
    int     jtagChainIrBits = 0;                        // 全链 IR 总位数
    uint8_t jtagCurIr       = 0;                        // DP 当前 IR；0 = 未知（需重选）
    // 链上每个 TAP 的 IDCODE（jtagIds[0] = 离 TDO 最近），由 JtagInitSequence 扫链填充。
    // ⚠ 必须留档：AGDI 的器件列表就是按本层给出的这张表画的（行数取
    //   CMSIS_DAP_JTAG_GetIDCODEs 的 *count、每行 IDCODE 取 GetDeviceIDList，
    //   两者都读 ctx->dapIdList，见 DetectTargetDapIdList 结尾）。
    //   只留 DP 一个 IDCODE 会让 Keil 的 JTAG 列表少一行（Todo.md §7，2026-10-03 实测）。
    uint32_t jtagIds[8]     = { 0 };
    // ⛔ JTAG **位流批量写**（原 JtagApWriteBurst）已作废，代码与开关一并删除。
    //    **不要再按"修采样点"这条路复活它** —— 它不是采样错位，是语义不成立。
    //
    // 依据（参考实现只有 orbtrace-1.4.3，其门级 DAP 用 Amaranth 写的）：
    //   `orbtrace/debug/cmsis_dap.py:712-716`
    //       with m.If(self.dbgif.ack==ACK_WAIT):
    //           self.retries.eq(self.retries-1),
    //           self.tfr_txb.eq(Mux(self.retries!=0, 6, 10))
    //     ⇒ ACK_WAIT（=2）时**回到状态 6 = 把同一笔 TRANSACT 原样重发**，
    //       `waitRetry` 默认 4096（同文件 :219）。即 WAIT 的含义是
    //       "这一笔**没完成**，必须重发同一笔"，而不是"上位机把 ACK 采错了位"。
    //
    //   实测数据也指向真 WAIT，而不是错位：
    //     `段ACK(段i=第i-1笔)=[2,1,2,1,2,1]`（严格交替）
    //     `RegAccessBlock: numRegs=29 -> [batch=0 single=20 wait=9]`
    //     —— 逐笔路径同样有 31% 的 WAIT（重试后才成功），说明该 AP 本来就常回 WAIT。
    //
    //   把 N 笔塞进**一条** ID_DAP_JTAG_SEQUENCE 连发，等于：返回 WAIT 的那半**被丢掉**
    //   （写没生效），而调用方又从上一次未确认处**整体重写**（写两遍 + AP TAR 多自增）
    //   ⇒ 丢字 + 错址叠加，正是 Keil 读/写内存出错、下载失败的成因。
    //   **结论：位流拼接式批量写在语义上不可能正确，没有"采样点"可修。**
    //
    // 合法提速方向（尚未打通）：让**能重发的层**去做批量 —— 固件门级的
    //   ID_DAP_TRANSFER（DAP_TransferMulti，内部按 WAIT 重发）。当前 JTAG 侧被
    //   `batchLimit = ctx->isSWD ? … : 0` 关掉，见 RegAccessBlock；要动须连
    //   JTAG "任何读都是延迟读"的规则一起处理（cmsis_dap.py:626-637、:778-786）。
    bool    jtagWritePending = false;                   // 有一次写已进 DP 流水线、待后续访问提交
    int     jtagRecoverFails = 0;                       // 链路自愈的连续失败次数（超过阈值停止重试，防风暴）
    uint32_t debugClock = kDefaultClock;
    bool isConnected = false;

    // SWD 链路自愈重入保护（见 SwdLinkRecover）
    bool swdRecovering = false;

    // Device info
    uint32_t capabilities = INFO_CAPS_SWD | INFO_CAPS_ATOMIC_CMDS;
    // ★ 探针自报的能力位（DAP_Info 0xF0：低字节 | 高字节 << 8）。RDDI_Open 时问一次并缓存。
    //   -1 = 没问出来（老固件可能不支持该命令）→ 各处沿用乐观口径（不按探针收口）。
    //   用途：CMSIS_DAP_Capabilities / StreamingTrace_GetSink* 判断 SWO 能力 ——
    //   本层是"探针 → 宿主"的中间层，不能替固件宣称它没有的能力（见两处调用点注释）。
    int probeCaps = -1;
    std::string productName;
    std::string serialNumber;
    std::string protocolVersion;

    // CMSIS-DAP 层的 DAP 标识列表（内容为目标的 IDCODE），
    // 仅由 CMSIS_DAP_DetectNumberOfDAPs 在连接目标后填充。
    // 注意：RDDI 层的 DAP_GetNumberOfDAPs / DAP_GetDAPIDList **不得**使用本列表，
    //       因为它们必须在连接目标之前返回结果（见 kSingleDapCount）。
    std::vector<uint32_t> dapIdList;

    // Error state
    int lastError = 0;
    std::string lastErrorStr;

    // Communication timeout
    int timeoutMs = 1000;

    // Trace state
    bool traceAttached = false;
    bool traceConnected = false;
    bool traceRunning = false;
    int traceMode = 0;  // 1=SWO, 2=ETM
    int traceBufferSize = 65536;
    int swoBaudrate = 115200;
    std::vector<uint8_t> traceBuffer;

    // ---- SWO 端口与传输方式（来自 AGDI 的配置串）----
    // AGDI 把用户在 µVision 的 Trace 页里选的东西拼成一条配置串交给本层，形如
    //   Trace=SWO-UART;TraceBaudrate=2000000;TraceTransport=Read;
    // 本层必须记住这两项，因为"用户选的端口/传输方式"决定实际下发什么：
    //   swoPortMode      : 0 = 未选 / Trace=Off（按 UART 处理），1 = SWO-UART，2 = SWO-Manchester
    //   swoTransportMode : 0 = 未选 / None，1 = Read（AGDI 轮询 CMSIS_DAP_SWO_Data），
    //                      2 = Stream（走 streaming sink 取数）
    // 用途：
    //   - Read 路径：CMSIS_DAP_SWO_Control(Start) 之前要按它下发 SWO Transport / Mode / Baudrate；
    //   - Stream 路径：StreamingTrace_Start 选 SWO Mode、StreamingTrace_GetSinkDetails 回 sink 类型。
    // 解析位置见 CMSIS_DAP_ConfigureDebugger。
    int swoPortMode      = 0;
    int swoTransportMode = 0;

    // SWO 波特率被探针拒绝的次数。AGDI 探测 SWO 时钟时会遍历分频（1..0x2000），
    // 每次都调一次 CMSIS_DAP_SWO_Baudrate；逐条记日志会把日志淹没，
    // 故只在头几次失败时留痕（见该函数的实现）。
    int swoBaudFailCount = 0;

    // ---- SWO 流式 trace 会话（Todo.md.bak §18.10-C 阶段 2）----
    // AGDI 的注册与取数协议（反汇编 CMSIS_AGDI.dll 逐调用点取证）：
    //   Connect(handle)                                  → GetSinkCount(handle,&n)
    //   → GetSinkDetails(handle,i,Rec) 循环 i=0..n-1     → Attach(handle,idx)
    //   → Start(handle,idx) → 循环 { SubmitEventBuffer(...) ; WaitForEvent(handle,idx,&token,50) }
    //   → Flush/Stop/Detach
    // 数据是"AGDI 提供缓冲、本层往里填、再用 token 通知 AGDI"，所以这里必须记住
    // AGDI 提交过来的条目与它期望的 token。
    // 本层只报 1 个 sink（0 号 = cmsis_dap_swo_trace），sinkIndex 只接受 0。
    int  traceSinkIndex = -1;                       // AGDI 选中的 sink（本层恒为 0）
    static const int kTraceMaxEntries = 64;         // 一次会话最多记住的提交条目数
    int   traceEntryToken[kTraceMaxEntries] = {0};  // 发给 AGDI 的 token（0 保留为"无事件"）
    void *traceEntryPtr[kTraceMaxEntries] = {nullptr};
    bool  traceEntryFilled[kTraceMaxEntries] = {false};
    int   traceEntryCount = 0;
    int   traceNextToken  = 1;
    int   traceEntryCapacityLogged = 0;             // 首次提交时打一次条目容量，便于核对
    int   traceAttachRefCount = 0;                  // Attach/Detach 引用计数（>0 = 会话在用）
    std::mutex traceMutex;                          // 保护上面的条目表（WaitForEvent 跨线程）

    // PC Sampling state
    bool pcSamplingEnabled = false;
    uint32_t pcSampleFrequency = 0;
    std::vector<uint32_t> pcSamples;      // PC 采样值
    std::vector<uint64_t> pcTimestamps;    // PC 采样时间戳
    int pcChannelCount = 1;                // 默认 1 个通道 (DWT PC Sample)
    struct ORBMDK_ITM_Decoder* itmDecoder = nullptr;  // ITM 解码器
    
    // Trace buffers
    std::vector<uint8_t> swoBuffer;        // SWO 数据缓冲区

    // Log callback（ARM rddi.h 签名：void (*)(void *context, const char *msg, int logLevel)）
    RDDILogCallback logCallback = nullptr;
    void           *logCallbackContext = nullptr;
    int             logCallbackMaxLevel = RDDI_LOGLEVEL_FATAL;

    // 状态 LED（DAP_HostStatus / ID_DAP_HOST_STATUS，见下方 SetHostLed 说明）
    uint32_t lastApTar      = 0;      // 最近一次写入的 AP TAR，用于识别 DHCSR 访问
    bool     hostConnectLed = false;  // Connect LED 当前状态（避免重复发包）
    bool     hostRunningLed = false;  // Running LED 当前状态
};

// ---------------------------------------------------------------------------
// 状态 LED 驱动（DAP_HostStatus / ID_DAP_HOST_STATUS）
//
// 官方 AGDI **从不调用** DAP_HostStatus —— 它的符号表里没有这个名字，反汇编
// 也找不到调用点。所以本层必须自己驱动，否则 orbtrace 上的 Connect / Running
// LED 永远不会亮（实测：烧录、调试时均不亮）。
//
//   Connect LED：连接目标成功时点亮；RDDI_Close / DAP_Disconnect 时熄灭。
//   Running LED：监视对 DHCSR(0xE000EDF0) 的写入 —— C_HALT(bit1) 置位表示
//                目标停机（Running 灭），清零表示目标运行（Running 亮）。
//                当前 AP 写地址由 ctx->lastApTar 跟踪（写 AP TAR 时更新）。
//
// 只在状态**变化**时发包，避免每个字一次 USB 往返。
// ---------------------------------------------------------------------------
static constexpr uint32_t kDhcsrAddr  = 0xE000EDF0u;
static constexpr uint32_t kDhcsrCHalt = 0x00000002u;   // C_HALT

enum { kHostLedConnect = 0, kHostLedRunning = 1 };

static void SetHostLed(RDDIContext* ctx, int type, bool on)
{
    bool* cache = (type == kHostLedConnect) ? &ctx->hostConnectLed : &ctx->hostRunningLed;
    if (*cache == on) {
        return;   // 状态未变化，不发包
    }

    const int rc = ORBMDK::DAP_HostStatus(static_cast<uint8_t>(type), on ? 1 : 0);
    if (rc == 0) {
        *cache = on;
        LOG_DEBUG("DAP_HostStatus: type=%d -> %d", type, on ? 1 : 0);
    } else {
        LOG_DEBUG("DAP_HostStatus: type=%d state=%d failed (rc=%d)", type, on ? 1 : 0, rc);
    }
}

// AP 写数据（DRW）时调用：若当前 TAR 指向 DHCSR，则据此更新 Running LED
static void TrackApDataWrite(RDDIContext* ctx, uint32_t value)
{
    if (ctx->lastApTar == kDhcsrAddr) {
        SetHostLed(ctx, kHostLedRunning, (value & kDhcsrCHalt) == 0);
    }
}

// AP 写地址（TAR）时调用：记住地址，供 TrackApDataWrite 判断
static void TrackApTarWrite(RDDIContext* ctx, uint32_t addr)
{
    ctx->lastApTar = addr;
}

// 使用 map + unique_ptr：句柄地址稳定，不受插入/删除影响
static std::map<RDDIHandle, std::unique_ptr<RDDIContext>> gContexts;

// 句柄必须从 1 开始分配，**绝不能分配 0**。
//
// Keil 官方 AGDI（RDDI_DAP_IF.cpp）把句柄当作指针使用：
//     RDDIHandle rddiHandle;              // 初值 0
//     status = rddi_Open(&rddiHandle, NULL);
//     ...
//     if (rddiHandle == NULL) return (RDDI_DAP_ERROR_INTERNAL);   // 每个函数入口都有
// elaphureLinkAGDI 同样用 `k_rddi_handle == NULL` 判断"尚未打开"。
//
// 因此若 RDDI_Open 返回 0：
//   - RDDI_DAP_Init 仍然成功（rddi_Open 返回 RDDI_SUCCESS，无任何报错弹窗）；
//   - 但后续 RDDI_DAP_Detect / RDDI_DAP_Product ... 全部在入口处
//     直接返回 RDDI_DAP_ERROR_INTERNAL，**根本不会调用到 CMSIS_DAP_Detect**；
//   - 结果是调试器适配器下拉框为空、且不弹任何错误框。
static RDDIHandle gNextHandle = 1;
static std::mutex gContextMutex;

// ============================================================================
// Helper Functions
// ============================================================================

// ---------------------------------------------------------------------------
// ARM RDDI 寄存器编号解码（rddi_dap.h）
//
// regID 的低 16 位是寄存器编号，bit16 = RnW(读标志)，bit17 = WaitForValue：
//   0..3  : DP 寄存器 A[3:2] = 0..3
//           (0x00 IDCODE/ABORT, 0x04 CTRL-STAT, 0x08 SELECT, 0x0C RDBUFF)
//   4..7  : AP 寄存器 A[3:2] = 0..3
//           (0x00 CSW, 0x04 TAR, 0x08 BASE, 0x0C DRW/IDR)，当前选中的 AP bank
//   8     : DP ABORT（写）
//   9     : JTAG IDCODE -> 读 DP 0x00
//   10    : DAP IDR    -> 读 DP 0x00
//   16/17 : MATCH_MASK / MATCH_RETRY，虚拟寄存器，不产生总线访问
//
// 历史问题：旧实现用 (regId >> 2) & 3 解码，并按 bit16 判断 AP —— 那是
// CMSIS-DAP DAP_Transfer 的 request 字节格式，不是 RDDI 的编号格式。结果：
//   regID 1/2/3 全部落到 DP 0x00，regID 4..7 被当成 DP 访问。
// AGDI（SWD.cpp/JTAG.cpp）全程按 0..7 编号调用 DAP_ReadReg/DAP_WriteReg，
// 因此所有非 DP0x00 的访问都取错寄存器，PDSC 调试序列与 Flash 下载随之失败。
//
// 另外：AP bank 由调用方通过 DAP_WriteReg(DP_SELECT) 自行管理（见 AGDI 的
// SWD_ReadAP/WriteAP），本层**不得**再改写 DP SELECT，否则会覆盖调用方设置的
// APSEL/APBANKSEL。
// ---------------------------------------------------------------------------
static inline int GetRegId(int regId)
{
    return regId & 0xFFFF;
}

static inline bool IsAPRegister(int regId)
{
    const int id = GetRegId(regId);
    return (id >= 4 && id <= 7);
}

// 返回 CMSIS-DAP DAP_Transfer 的 request 字节：bit0 = APnDP，bits[2:1] = A[3:2]。
// 返回 -1 表示该编号不受支持（调用方应回 RDDI_DAP_BAD_REGISTER_ID）。
static inline int GetRegOffset(int regId)
{
    const int id = GetRegId(regId);

    if (id >= 0 && id <= 3) {
        return kDapRegOffsetMap[id];             // DP: A[3:2] = id
    }
    if (id >= 4 && id <= 7) {
        return kDapRegOffsetMap[4 + (id - 4)];   // AP: A[3:2] = id - 4
    }
    if (id == 8 || id == 9 || id == 10) {
        return kDapRegOffsetMap[0];              // ABORT / JTAG IDCODE / DAP IDR
    }
    return -1;
}

// 虚拟寄存器（MATCH_MASK/MATCH_RETRY）不是真实总线访问
static inline bool IsVirtualReg(int regId)
{
    const int id = GetRegId(regId);
    return (id == 16 || id == 17);
}

// 获取有效的上下文指针（返回引用，保证地址稳定）
static inline RDDIContext* GetContext(RDDIHandle handle)
{
    std::lock_guard<std::mutex> lock(gContextMutex);
    auto it = gContexts.find(handle);
    if (it != gContexts.end()) {
        return it->second.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 块传输能力探测（每个上下文只做一次）
//
// 探测载荷选 **读 DP IDCODE**：只读、无副作用，且与既有 SWD_ReadID 的读法一致。
//   - 固件支持 ID_DAP_TRANSFER_BLOCK：返回 OK，且 Transfer Count 与请求一致；
//   - 固件不支持：该命令被当作未知命令处理，响应长度 / 命令 ID / 计数对不上，
//     DAP_TransferBlock 会返回错误（含新增的 Transfer Count 校验）→ 判定不可用。
//
// ★ 两段式：先单字，再**按实际工作深度**多字读一遍并逐字校验。
//   只测单字太浅 —— 固件只要"能回一个字"就会通过，而块传输真正被用到的深度是
//   BlockWordsLimit()（V2 下 125/126 字/往返，见 DAP_RegWriteRepeat）。固件若把 count
//   解释错、只回部分数据或响应被截断，会**静默**给出错误数据（命令全报 OK、值却是错的）。
//   故这里把深度测满：n 个字都应等于同一个 IDCODE。
//   读 DP IDCODE 是理想的压力载荷 —— DP 读不自增、无副作用，n 次读恒回同一值。
//
// 探测失败只影响性能，不影响功能 —— 之后永久走逐字 DAP_Transfer。
// ---------------------------------------------------------------------------
static bool ProbeBlockTransfer(int dapId)
{
    // ① 单字基线。
    uint32_t idcode = 0;
    const int status = ORBMDK::DAP_TransferBlock(dapId, 1, 0x02 /* read IDCODE */,
                                                 nullptr, &idcode);
    if (status != ORBMDK::DAP_RES_OK) {
        LOG_WARN("Block transfer probe failed (status=%d) -> firmware does not support "
                 "ID_DAP_TRANSFER_BLOCK, falling back to single transfers", status);
        return false;
    }

    // ② 按实际工作深度多字读，逐字校验。
    const int n = BlockWordsLimit();
    if (n > 1) {
        std::vector<uint32_t> buf(static_cast<size_t>(n), 0);
        const int deep = ORBMDK::DAP_TransferBlock(dapId, static_cast<uint16_t>(n),
                                                   0x02, nullptr, buf.data());
        if (deep != ORBMDK::DAP_RES_OK) {
            LOG_WARN("Block transfer probe: %d-word block read failed (status=%d) -> "
                     "固件在真实工作深度下不可用，回退逐字传输", n, deep);
            return false;
        }
        for (int i = 0; i < n; ++i) {
            if (buf[i] != idcode) {
                LOG_WARN("Block transfer probe: %d-word block read 第 %d 字不符 "
                         "(0x%08X != IDCODE 0x%08X) -> 固件深层块读静默不可信，"
                         "回退逐字传输", n, i, buf[i], idcode);
                return false;
            }
        }
    }

    LOG_INFO("Block transfer probe OK (ID_DAP_TRANSFER_BLOCK supported, idcode=0x%08X, "
             "deep %d-word read verified)", idcode, n);
    return true;
}

// 首次用到块传输时调用；之后再调用无开销。
//
// **JTAG 放弃块传输**（ID_DAP_TRANSFER_BLOCK 由固件驱动 TAP）：JTAG 的 IR/DR
// 时序全部由本层引擎生成（JtagSetIr + JtagDrScan），并缓存了 IR 假说（jtagCurIr）
// 与 posted-write 流水线（jtagWritePending）；固件驱动的块传输与它是两条各自
// 维护状态的通路，共用同一条链时交接边界难以保证一致。故 JTAG 会话直接落定
// "不支持块传输"，永久走逐字引擎 DapTransferFor —— 功能等价，只是慢。
//
// SWD 走"先探测、失败即永久回退"：探测失败只影响性能，功能不受影响，之后
// 永久走逐字 DAP_Transfer。
//
// ---------------------------------------------------------------------------
// 速率计量：把"当前传输层快照"报给日志模块（TESTSPEED 级）
//
// 只在计量开启时做事 —— 阈值高于 TESTSPEED 时**第一行就返回**（零开销，不取时钟）。
// 传输层 / 速度 / 出包字节 / 每次往返字数 / 块传输是否生效，任一变化都会重打配置行。
// ---------------------------------------------------------------------------
static void PublishMeterConfig(const RDDIContext* ctx)
{
    if (!ORBMDK_LogMeterEnabled()) {
        return;
    }

    const int pkt   = ORBMDK_USB_Bulk_GetMaxCommandBytes();  // 0 = HID / 未连接
    const int speed = ORBMDK_USB_Bulk_GetDeviceSpeed();      // 0 = HID；1=Low 2=Full 3=High
    const bool bulk = (pkt > 0);

    int linkMbps = 12;                     // V1 HID：Full Speed 中断端点（负载固定 64 字节）
    if (bulk) {
        linkMbps = (speed == 3) ? 480 : (speed == 2 ? 12 : (speed == 1 ? 2 : 0));
    }

    ORBMDK_LogMeterSetTransport(bulk ? "V2/Bulk" : "V1/HID", linkMbps,
                                bulk ? pkt : 64, BlockWordsLimit(),
                                ctx->blockTransferSupported ? 1 : 0);
}

// 传输层是 HID 还是 Bulk 与本函数无关：两者都经 ORBMDK_HID_DAPCommand 派发，
// 字数上限由 BlockWordsLimit() 按当前出包长度给出。
static void EnsureBlockTransferProbed(RDDIContext* ctx, int dapId)
{
    if (ctx->blockTransferProbed) {
        return;
    }
    ctx->blockTransferProbed = true;

    if (!ctx->isSWD) {
        // JTAG：不使用块传输（见上），无需探测固件能力。
        ctx->blockTransferSupported = false;
        LOG_INFO("EnsureBlockTransferProbed: JTAG 会话 -> 不使用 ID_DAP_TRANSFER_BLOCK，"
                 "走逐字传输");
        return;
    }

    ctx->blockTransferSupported = ProbeBlockTransfer(dapId);
}

// ============================================================================
// RDDI Core Functions
// ============================================================================

// 进程级递增的"第几次打开"计数器。
//
// 不能用普通 static：**AGDI 在两次 rddi_Open 之间会卸载并重新加载本 DLL**
// （实测日志里重载后全局状态归零），static 每次都从 1 开始 —— 于是日志里
// 满屏"会话 #1"，分隔标记就失去了区分能力（2026-09-30 实测）。
// 命名共享内存（Local\ 作用域 = 当前会话）能跨重载保留，DLL 卸载后视图仍在
// 进程里存活，所以重载后重新映射拿到的还是同一个计数器。
static unsigned _nextOpenSeq(void)
{
    static volatile LONG* slot = nullptr;

    if (!slot) {
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                        0, sizeof(LONG), "Local\\ORBMDK_OpenSeq");
        if (map) {
            void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));
            if (view) {
                slot = (volatile LONG*)view;
            }
        }
    }
    return slot ? (unsigned)InterlockedIncrement(slot) : 1u;
}

RDDI_FUNC int RDDI_Open(RDDIHandle *pHandle, const void *pDetails)
{
    // 会话分隔标记：日志是**跨进程、跨会话追加**的（每次 Keil 会话一个新 UV4 进程，
    // 同一进程里 AGDI 又会反复 Open 约 7 次）。没有醒目分隔行时，排查要靠时间戳
    // 反推"这一段属于哪次会话" —— 实测非常费时。用 INFO 级，正常排障就能看到。
    LOG_INFO("================ 会话 #%u 开始 (pid=%lu) ================",
             _nextOpenSeq(), (unsigned long)GetCurrentProcessId());
    LOG_DEBUG("RDDI_Open: enter, pDetails=%p", pDetails);

    if (!pHandle) {
        return RDDI_BADARG;
    }

    std::lock_guard<std::mutex> lock(gContextMutex);

    // 打开设备：此刻用户还没选接口（AGDI 要到 ConfigureInterface 才把选择告诉我们），
    // 所以按"哪个能用用哪个"打开：V2(Bulk) 优先，打不开才 V1(HID)。
    // 用户一旦在对话框里选定，CMSIS_DAP_ConfigureInterface 会把传输层切过去，
    // 并且**不再允许**两条通道互相顶替（选了 V2 就必须是 V2，打不开就报错）。
    //
    // 注意**不要**把这件事放进 ORBMDK_HID_OpenDevice：它会持有 g_hidMutex，
    // 而回退路径又会调用 ORBMDK_HID_OpenDevice，std::mutex 非递归会死锁。
    {
        const USB_Bulk_Mode cur = ORBMDK_USB_Bulk_GetMode();
        const int pref = ORBMDK_USB_Bulk_GetTransportPreference();
        const bool switchNeeded =
            (pref == USB_BULK_TRANSPORT_BULK && cur != USB_BULK_BULK_MODE) ||
            (pref == USB_BULK_TRANSPORT_HID  && cur != USB_BULK_HID_MODE);
        const bool firstOpen = (cur == USB_BULK_NOT_INITED &&
                               !ORBMDK::ORBMDK_HID_IsConnected());

        if (firstOpen || switchNeeded) {
            // 多调试器：AGDI 的 pDetails 是 NULL（实测），**不提供**"用哪一台"，
            // 所以按枚举顺序取第一台 —— 本层不再提供任何"指定序列号"的隐藏开关。
            LOG_DEBUG("RDDI_Open: opening transport (current=%d, pref=%d, switch=%d)...",
                      (int)cur, pref, (int)switchNeeded);
            if (ORBMDK_USB_Bulk_Init(0, 0, nullptr) != 0) {
                // 带上系统错误码：否则现场只能看到"失败了"，无从判断是
                // 设备未插、被独占、还是驱动问题。
                const DWORD sysErr = GetLastError();
                LOG_WARN("RDDI_Open: open failed (V2 Bulk and V1 HID both failed), "
                          "GetLastError()=%lu", (unsigned long)sysErr);
                // 失败时必须把句柄置 0（本层句柄从 1 开始，0 恒为无效值）：
                // AGDI 的 `rddi_Open(&handle, NULL)` 之后直接用该变量当句柄，
                // 不写的话它会保留上一次成功打开时的旧句柄，继续对着一个
                // 已失效的连接调用各接口。
                *pHandle = 0;
                return RDDI_FAILED;
            }
        }
    }

    // 分配新句柄
    RDDIHandle handle = gNextHandle++;

    // 创建新的上下文（unique_ptr 保证生命周期）
    auto ctx = std::make_unique<RDDIContext>();
    ctx->initialized = true;
    ctx->handle = handle;
    ctx->isConnected = true;
    ctx->blockTransferSupported = true;   // 乐观开启，首次用到时按 SWD/JTAG 落定

    // 从 HID 层取设备标识（USB 产品名 / 序列号）。
    // 历史问题：这两个字段此前从未被赋值，导致 CMSIS_DAP_Identify 只能回退到
    // 硬编码串、CMSIS_DAP_GetDeviceIDList 返回空字符串、CMSIS_DAP_GetGUID 得到
    // "ORBTrace-"。数据其实早已由 HidD_GetProductString / HidD_GetSerialNumberString
    // 取到，只是没有向上传递。
    // 注：版本**不从这里取** —— 必须问设备（`DAP_Info(0x04)`，即 CMSIS-DAP 协议版本），
    // 见函数末尾那段。
    {
        char product[128] = {};
        char serial[128] = {};
        if (ORBMDK::ORBMDK_HID_GetDeviceInfo(product, sizeof(product),
                                             serial, sizeof(serial),
                                             nullptr, 0) == 0) {
            ctx->productName  = product;
            ctx->serialNumber = serial;
            LOG_DEBUG("RDDI_Open: device product='%s' serial='%s'",
                      ctx->productName.c_str(), ctx->serialNumber.c_str());
        } else if (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) {
            // V2 模式下本来就没打开 HID 接口，这里取不到属正常，
            // 产品名/序列号改从 Bulk 接口与设备描述符取（见下）
            LOG_DEBUG("RDDI_Open: no HID device open (V2 mode), "
                      "product/serial are taken from the Bulk interface");
        } else {
            LOG_WARN("RDDI_Open: ORBMDK_HID_GetDeviceInfo failed");
        }

        // V2 模式下产品名必须取 **Bulk 接口** 自己的字符串描述符。
        //
        // Keil 对话框里的适配器名字就是 CMSIS_DAP_Identify(idNo=2) 的返回值，
        // 而 HID 层给出的是 HID 接口的名字（"CMSIS-DAP v1"）—— 即使实际已经
        // 走 V2 传输，对话框仍会显示 v1（实测现象）。
        // 注："CMSIS-DAP v" 这个字面量在 AGDI 与官方 RDDI DLL 里都不存在
        // （已用字节搜索确认），该名字只能来自设备描述符。
        if (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) {
            char bulkProduct[128] = {};
            if (ORBMDK_USB_Bulk_GetProductName(bulkProduct, sizeof(bulkProduct)) == 0) {
                ctx->productName = bulkProduct;
            } else {
                LOG_WARN("RDDI_Open: V2 interface string unavailable, keeping '%s'",
                         ctx->productName.c_str());
            }

            // 序列号：V2 下没有打开 HID 接口，HidD_GetSerialNumberString 取不到，
            // 改从设备的 iSerialNumber 字符串描述符读。不做的话
            // CMSIS_DAP_Identify(idNo=3) 只能返回 "Unknown"，
            // µVision 适配器列表里看不到序列号。
            char bulkSerial[128] = {};
            if (ORBMDK_USB_Bulk_GetSerialNumber(bulkSerial, sizeof(bulkSerial)) == 0) {
                ctx->serialNumber = bulkSerial;
            }

        }
    }

    // ---- 上报给宿主的版本串 ----
    // 唯一来源 = **设备自报的 DAP_Info(0x04)**（走已连上的那条传输；DAP_GetInfo
    // 内部按当前模式分发到 HID 或 Bulk）。**不读** USB 描述符的 bcdDevice：
    // 那是 USB 栈/引导写的字段，与版本无关（见 kFallbackProtocolVersion 上方）。
    //
    // ⚠️ 按 CMSIS-DAP v2.1.2，0x04 是 **CMSIS-DAP 协议版本**（"2.1.0" 形态），不是产品
    //    固件版本（0x09）。AGDI 的 `cmp eax, 2` 门控要的正是协议版本，故问 0x04 是对的。
    //
    // 必须在这里问一次并缓存：AGDI 会多次调 Identify(idNo=4) 并拿返回值做能力
    // 判定，每次现问设备会给出不一致的串，而且设备可能已经断开。
    //
    // 归一化不能省：AGDI 对主版本做 `cmp eax, 2`（§17.6），只有主版本 >= 2 才会进入
    // 它的 streaming sink 注册分支。当前口径是"提升到 >= 2"（2026-10-01 放开，§18.10-C），
    // 与 caps 的 0x40 同侧；问不到设备时由归一化落到兜底 "1.0.0"（主版本 1 = 门控**关闭**，
    // 保守侧，理由见 kFallbackProtocolVersion 上方）。
    {
        char deviceVersion[64] = {};
        const int rc = ORBMDK::DAP_GetInfo(DAP_INFO_PROTOCOL_VERSION, deviceVersion,
                                           sizeof(deviceVersion));
        char reported[64] = {};
        ORBMDK_NormalizeProtocolVersion(deviceVersion, reported, sizeof(reported));
        ctx->protocolVersion = reported;
        LOG_INFO("RDDI_Open: device DAP_Info(0x04)='%s' (rc=%d) -> Identify(4) reports '%s'",
                 deviceVersion, rc, ctx->protocolVersion.c_str());

        // 诊断（核对 CMSIS-DAP v2.1.2 规范用，**DEBUG 级**）：0x04 是**协议版本**，
        // 0x09 才是**产品固件版本**。两者都问一遍，用来判断设备到底提供哪一个
        // （实测 H7-TOOL：0x04 回 "2.0.0"、0x09 回 Len=0 —— 不提供）。
        char devFwVer[64] = {};
        const int rc9 = ORBMDK::DAP_GetInfo(DAP_INFO_FW_VERSION, devFwVer, sizeof(devFwVer));
        LOG_DEBUG("RDDI_Open: DAP_Info id 对照 -> 0x04 protocol='%s'(rc=%d) / 0x09 firmware='%s'(rc=%d)",
                  deviceVersion, rc, devFwVer, rc9);
    }

    // ---- 探针自报能力位（DAP_Info 0xF0）：SWO/流式能力必须按它收口 ----
    // 与固件版本串同理，问一次就缓存：CMSIS_DAP_Capabilities 与 StreamingTrace_*
    // 都要用它，而 AGDI 的调用时机不确定（设备可能已经断开）。
    // 读不到不是致命错误：保持 -1，各处沿用乐观口径（见 ctx->probeCaps 注释）。
    {
        uint16_t caps = 0;
        if (ORBMDK::DAP_GetCapabilities(&caps) == 0) {
            ctx->probeCaps = static_cast<int>(caps);
            LOG_INFO("RDDI_Open: probe capabilities = 0x%04X "
                     "(SWD=%d JTAG=%d SWO_UART=%d SWO_MANCHESTER=%d ATOMIC=%d TIMER=%d SWO_STREAM=%d UART=%d USB_COM=%d)",
                     static_cast<unsigned>(caps),
                     (caps & INFO_CAPS_SWD) ? 1 : 0,
                     (caps & INFO_CAPS_JTAG) ? 1 : 0,
                     (caps & INFO_CAPS_SWO_UART) ? 1 : 0,
                     (caps & INFO_CAPS_SWO_MANCHESTER) ? 1 : 0,
                     (caps & INFO_CAPS_ATOMIC_CMDS) ? 1 : 0,
                     (caps & INFO_CAPS_TEST_DOMAIN_TIMER) ? 1 : 0,
                     (caps & INFO_CAPS_SWO_STREAMING_TRACE) ? 1 : 0,
                     (caps & INFO_CAPS_UART_PORT) ? 1 : 0,
                     (caps & INFO_CAPS_USB_COM_PORT) ? 1 : 0);
        } else {
            LOG_WARN("RDDI_Open: DAP_Info(0xF0) 未答上来 -> 能力位沿用乐观口径（不按探针收口）");
        }
    }

    // 插入 map（堆分配，地址稳定）
    gContexts.emplace(handle, std::move(ctx));

    *pHandle = handle;

    return RDDI_SUCCESS;
}

RDDI_FUNC int RDDI_Close(RDDIHandle handle)
{
    LOG_DEBUG("RDDI_Close: enter, handle=%d", handle);
    std::lock_guard<std::mutex> lock(gContextMutex);

    auto it = gContexts.find(handle);
    if (it == gContexts.end()) {
        LOG_WARN("RDDI_Close: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    // 熄灭状态 LED（官方 AGDI 不调用 DAP_HostStatus，须由本层驱动）
    if (it->second) {
        SetHostLed(it->second.get(), kHostLedConnect, false);
        SetHostLed(it->second.get(), kHostLedRunning, false);
    }

    // 从 map 中移除并自动释放
    gContexts.erase(it);

    // 记日志是为了能确认"AGDI 到底有没有回收实例"以及 context 是否累积。
    // 注意 gNextHandle 只增不减（句柄编号不复用），因此同一进程内反复
    // RDDI_Open 而不 RDDI_Close 会让 gContexts 持续增长。
    LOG_DEBUG("RDDI_Close: handle=%d closed, remaining contexts=%d",
              handle, (int)gContexts.size());
    return RDDI_SUCCESS;
}

// 标准 RDDI_GetLastError 签名：无 handle 参数
RDDI_FUNC int RDDI_GetLastError(int *pError, char *pDetails, size_t detailsLen)
{
    LOG_DEBUG("RDDI_GetLastError: enter, pError=%p, pDetails=%p, detailsLen=%llu",
              (void *)pError, (void *)pDetails, (unsigned long long)detailsLen);
    // 获取最后一个有效的上下文
    RDDIContext* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(gContextMutex);
        if (!gContexts.empty()) {
            ctx = gContexts.rbegin()->second.get();
        }
    }

    if (!ctx) {
        LOG_WARN("RDDI_GetLastError: no open context");
        return RDDI_FAILED;
    }

    if (pError) {
        *pError = ctx->lastError;
    }

    if (pDetails && detailsLen > 0) {
        strncpy_s(pDetails, detailsLen, ctx->lastErrorStr.c_str(), detailsLen - 1);
        pDetails[detailsLen - 1] = '\0';
    }

    LOG_DEBUG("RDDI_GetLastError: err=%d, details='%s'",
              ctx->lastError, ctx->lastErrorStr.c_str());
    return RDDI_SUCCESS;
}

// ============================================================================
// DAP Functions
// ============================================================================

RDDI_FUNC int DAP_GetInterfaceVersion(const RDDIHandle handle, int *version)
{
    LOG_DEBUG("DAP_GetInterfaceVersion: enter, handle=%d", handle);
    if (version) {
        *version = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!version) {
        return RDDI_BADARG;
    }

    *version = 1;  // RDDI version 1
    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_Configure(const RDDIHandle handle, const char *configFileName)
{
    LOG_DEBUG("DAP_Configure: enter, handle=%d, cfgFile=%p", handle, (const void *)configFileName);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // configFileName is usually nullptr for CMSIS-DAP
    return RDDI_SUCCESS;
}

// 标准 RDDI_DAP_CONN_DETAILS 结构体
RDDI_FUNC int DAP_Connect(const RDDIHandle handle, RDDI_DAP_CONN_DETAILS *pConnDetails)
{
    LOG_DEBUG("DAP_Connect: enter, handle=%d, pConnDetails=%p", handle, pConnDetails);

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("DAP_Connect: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    if (pConnDetails) {
        // 填充实现者信息
        strncpy_s(pConnDetails->implementorName, sizeof(pConnDetails->implementorName),
                   "ORBTrace", _TRUNCATE);
        strncpy_s(pConnDetails->connectionDescription, sizeof(pConnDetails->connectionDescription),
                   "ORBTrace CMSIS-DAP over USB HID", _TRUNCATE);
        LOG_DEBUG("DAP_Connect: connected to %s", pConnDetails->connectionDescription);
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_GetNumberOfDAPs(const RDDIHandle handle, int *noOfDAPs)
{
    LOG_DEBUG("DAP_GetNumberOfDAPs: enter, handle=%d", handle);
    if (noOfDAPs) {
        *noOfDAPs = 0;  // 即使失败也写入输出参数，避免调用方读到未初始化内存
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 按 ARM rddi_dap.h：本函数在 DAP_Connect 之前调用，且"不得与目标通信"。
    // ORBTrace 为单 DAP 设备，DAP ID 恒为 0。
    // (历史问题：曾返回 dapIdList.size()，而该列表需连接目标后才填充 → 恒为 0，
    //  导致 Keil 报 "No Debug Unit Found")
    *noOfDAPs = kSingleDapCount;
    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_GetDAPIDList(const RDDIHandle handle, int *DAP_ID_Array, size_t sizeOfArray)
{
    LOG_DEBUG("DAP_GetDAPIDList: enter, handle=%d, array=%p, sizeOfArray=%llu",
              handle, (void *)DAP_ID_Array, (unsigned long long)sizeOfArray);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!DAP_ID_Array || sizeOfArray == 0) {
        LOG_WARN("DAP_GetDAPIDList: BADARG (array=%p, sizeOfArray=%llu)",
                 (void *)DAP_ID_Array, (unsigned long long)sizeOfArray);
        return RDDI_BADARG;
    }

    // ⚠️ sizeOfArray 的两种解释都必须接受（官方按**字节数**、AGDI 的"多 DAP"
    // 分支按**元素个数**，见 COMPAT_ANALYSIS §10.3(6)）。旧实现写死
    // `sizeOfArray < sizeof(int) → RDDI_BADARG`：传 1 时直接失败，而且是
    // **静默**失败（不发任何 USB 命令、也不写输出参数），现场表现为
    // "CMSIS_DAP_Connect 成功之后立刻 Disconnect/Close，Debug Settings 里
    // 显示 RDDI-DAP Error"。本函数只回一个 DAP ID，不做容量换算。

    // 与 DAP_GetNumberOfDAPs 保持一致：连接目标之前即可返回，单 DAP，ID = 0。
    // 注意 DAP_ID 会被后续 DAP_ReadReg / DAP_WriteReg 当作 CMSIS-DAP 的
    // "DAP Index" 字节使用，必须是**索引**而不是 IDCODE（写 IDCODE 会让
    // 低字节变成非法的 DAP Index）。
    DAP_ID_Array[0] = kSingleDapId;

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_Disconnect(const RDDIHandle handle)
{
    LOG_DEBUG("DAP_Disconnect: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    ORBMDK::DAP_DisconnectTarget();
    ctx->isConnected = false;
    return RDDI_SUCCESS;
}

// ============================================================================
// Register Access Functions
// ============================================================================

// ---------------------------------------------------------------------------
// SWD 链路自愈
//
// 实测：目标一旦对某条传输回 NO_ACK（Transfer Response = 0x07），之后**所有**
// DAP_Transfer 都返回 NO_ACK —— 目标的 SWD 进了"协议错误/静默"状态。这种状态
// **只有线复位能解除**（写 ABORT 不够）；而 AGDI 的恢复动作只是"写 ABORT 后重试"，
// 于是它一路重试一路失败，最后报 "RDDI-DAP Error"。
//
// 所以本层自己补一次标准恢复序列（与 OpenOCD 的 dap_dp_init 同构）：
//     line reset → JTAG-to-SWD → line reset → 写 ABORT(0x1E) → 读 DPIDR 校验
// 恢复成功后把调用方那次传输重放一次。恢复期间用 ctx->swdRecovering 防重入。
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// 主机侧 JTAG 引擎（COMPAT_ANALYSIS §18.9）
//
// orbtrace 固件提供的是"通用位流"命令 ID_DAP_JTAG_SEQUENCE(0x14)（见 DAP_JTAG_Sequence），
// 于是 ADIv5 的"选 IR / 组 35 位 DR / 处理 posted read"这些语义**全在本层实现**，
// 不依赖固件那套高层 JTAG 命令（其内部 IR 长度表在本目标上没能配对成功）。
//
// 位序权威来源：orbtrace-1.4.3/daplink/cmsis-dap/{JTAG_DP.c, DAP.c}
//   JTAG_IR():       TDI = [TDO 侧器件旁路 1×ir_before][目标 IR, LSB first][TDI 侧旁路 1×ir_after]
//                    TAP：2×TMS=1 进 → 2×TMS=0 移位 → 2×TMS=1 出 → 1×TMS=0 回 Idle
//   JTAG_Transfer(): DR 移位序 = [RnW][A2][A3][D0..D31]；TDO 先出 3 位 ACK，
//                    ack = (c0<<1)|(c1<<0)|(c2<<2)（OK=1 WAIT=2 FAULT=4）
//   DAP.c DAP_JTAG_Transfer(): 读是 **posted** 的 —— 先发请求、再读 DP_RDBUFF 取数，
//                    且读 RDBUFF 前 IR 必须切回 DPACC；写也要一次后续访问来提交。
//
// 链序约定（与固件一致）：ir_length[0] = 离 TDO 最近的器件，DR 扫描时靠 TDO 的旁路位先出。
// DP 在链上的位置由扫链结果自动判定，不依赖 OpenOCD 的声明顺序。
// ---------------------------------------------------------------------------

// ADIv5 JTAG-DP 的 IR 指令码（对应 DAP.h 的 JTAG_ABORT/DPACC/APACC/IDCODE/BYPASS）
enum : uint8_t {
    kJtagIrAbort  = 0x08,
    kJtagIrDpacc  = 0x0A,
    kJtagIrApacc  = 0x0B,
    kJtagIrIdcode = 0x0E,
    kJtagIrBypass = 0x0F,
};

// 线上 ACK -> ORBMDK 返回码
static inline int JtagAckToRes(uint8_t ack)
{
    switch (ack) {
    case 1:  return ORBMDK::DAP_RES_OK;      // DAP_TRANSFER_OK
    case 2:  return ORBMDK::DAP_RES_WAIT;    // DAP_TRANSFER_WAIT
    case 4:  return ORBMDK::DAP_RES_FAULT;   // DAP_TRANSFER_FAULT
    default: return ORBMDK::DAP_RES_NO_ACK;
    }
}

// 位缓冲读写（LSB first：第 0 位 = 第一个时钟沿）
static void JtagPackBits(uint8_t* buf, int* pos, uint32_t value, int bits)
{
    for (int i = 0; i < bits; ++i) {
        if ((value >> i) & 1u) {
            const int p = *pos + i;
            buf[p >> 3] |= static_cast<uint8_t>(1u << (p & 7));
        }
    }
    *pos += bits;
}

static uint32_t JtagUnpackBits(const uint8_t* buf, int* pos, int bits)
{
    uint32_t v = 0;
    const int n = (bits > 32) ? 32 : bits;
    for (int i = 0; i < n; ++i) {
        const int p = *pos + i;
        if ((buf[p >> 3] >> (p & 7)) & 1u) v |= (1u << i);
    }
    *pos += bits;
    return v;
}

// JTAG 位流诊断：把字节数组转成十六进制串（仅用于日志）。
static std::string JtagHex(const uint8_t* buf, int n)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string s;
    if (!buf || n <= 0) return s;
    s.reserve(static_cast<size_t>(n) * 2);
    for (int i = 0; i < n; ++i) {
        s += kHex[buf[i] >> 4];
        s += kHex[buf[i] & 0x0F];
    }
    return s;
}

// TAP 复位：TMS=1 连续 6 拍（>5 保证进 Test-Logic-Reset）后回 Run-Test/Idle。
static bool JtagTapReset(void)
{
    const ORBMDK::JtagSeg segs[] = {
        { 6, 1, 0, nullptr },
        { 1, 0, 0, nullptr },
    };
    return ORBMDK::DAP_JTAG_Sequence(segs, 2, nullptr, 0, nullptr) == 0;
}

// 扫链：TAP 复位后所有 TAP 的 DR 装载 IDCODE（BYPASS 器件移出 0）。
// ids[0] = 离 TDO 最近的器件，与 ir_length[0] 的序一致。返回链上器件数。
static int JtagScanChain(uint32_t* ids, int maxIds)
{
    if (!ids || maxIds <= 0) return 0;
    memset(ids, 0, sizeof(uint32_t) * static_cast<size_t>(maxIds));
    if (!JtagTapReset()) return 0;

    // Idle -> Select-DR -> Capture-DR -> Shift-DR，最多移出 128 位（4 个 TAP；
    // 单段上限 64 位，故拆两段）。多移的位只是链上 BYPASS 器件/末端的 0。
    const ORBMDK::JtagSeg segs[] = {
        { 1, 1, 0, nullptr },
        { 2, 0, 0, nullptr },
        { 64, 0, 1, nullptr },
        { 64, 0, 1, nullptr },
        { 2, 1, 0, nullptr },   // Exit1-DR, Update-DR
        { 1, 0, 0, nullptr },   // -> Run-Test/Idle
    };
    uint8_t tdo[16] = {0};
    if (ORBMDK::DAP_JTAG_Sequence(segs, 6, tdo, sizeof(tdo), nullptr) != 0) return 0;

    int count = 0;
    for (int i = 0; i < 4 && i < maxIds; ++i) {
        int bit = i * 32;
        const uint32_t id = JtagUnpackBits(tdo, &bit, 32);
        ids[i] = id;
        // 有效 IDCODE：bit0 = 1（IEEE1149.1 约定）；0/全 1 表示该位置无器件
        if ((id & 1u) && id != 0xFFFFFFFFu) count = i + 1;
    }
    return count;
}

// 已知 IDCODE -> IR 位数（掩掉版本位 [31:28]，故 0x2/0x4/0x6BA00477 都能命中）
static int JtagIrLenForId(uint32_t id)
{
    switch (id & 0x0FFFFFFFu) {
    case 0x0BA00477u: return 4;   // ARM CoreSight JTAG-DP（Cortex-M）
    case 0x06413041u: return 5;   // STM32F4 边界扫描 TAP
    default:          return 0;   // 未知：调用方按 4 位兜底并告警
    }
}

// 选 IR：目标器件送 ir，其余器件全部 BYPASS(1)。IR 已对则直接返回（省一次往返）。
static bool JtagSetIr(RDDIContext* ctx, uint8_t ir)
{
    if (ctx->jtagCurIr == ir) return true;
    if (ctx->jtagChainCount <= 0 || ctx->jtagDpIndex < 0) return false;

    const int idx = ctx->jtagDpIndex;
    int before = 0, after = 0;
    for (int i = 0; i < idx; ++i) before += ctx->jtagIrLens[i];
    for (int i = idx + 1; i < ctx->jtagChainCount; ++i) after += ctx->jtagIrLens[i];

    const int irLen = ctx->jtagIrLens[idx];
    const int total = before + irLen + after;
    if (total <= 0 || total > 64) {
        LOG_ERROR("JtagSetIr: 全链 IR 位数 %d 超出单段上限 64", total);
        return false;
    }

    uint8_t tdi[8] = {0};
    int pos = 0;
    for (int i = 0; i < before; ++i) { tdi[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7)); ++pos; }
    JtagPackBits(tdi, &pos, ir, irLen);
    for (int i = 0; i < after; ++i) { tdi[pos >> 3] |= static_cast<uint8_t>(1u << (pos & 7)); ++pos; }

    LOG_DEBUG("JtagSetIr: ir=0x%02X len=%d total=%d before(TDO侧)=%d after(TDI侧)=%d tdi=%s",
              ir, irLen, total, before, after, JtagHex(tdi, (total + 7) / 8).c_str());

    // ⚠️ IEEE 1149.1：Shift-IR -> Exit1-IR 的那一拍**同时也要移位**，所以
    //    "移 total 位" 必须正好用 total 拍，且**最后一拍的 TMS 必须是 1**。
    //    参考固件 JTAG_DP.c:JTAG_IR_Function() 的 else 分支：
    //        PIN_TMS_SET(); JTAG_CYCLE_TDI(ir);   /* Set last IR bit & Exit1-IR */
    //    旧实现是"先移 total 拍 TMS=0，再补 {2,1} 退出"，多出来的那一拍把整个 IR
    //    又推了一位：DP 收到的指令由 0xA(DPACC) 旋转成 0xD（非法）→ JTAG-DP 退化为
    //    BYPASS → DR 只剩 1 位旁路 → TDO 整段恒 0（这正是 DPACC 恒 0 的根因）。
    const int headBits = total - 1;
    const int lastBit  = (tdi[(total - 1) >> 3] >> ((total - 1) & 7)) & 1u;
    const uint8_t lastByte = static_cast<uint8_t>(lastBit);

    ORBMDK::JtagSeg segs[6];
    int n = 0;
    segs[n++] = { 2, 1, 0, nullptr };                          // Idle -> Select-DR -> Select-IR
    segs[n++] = { 2, 0, 0, nullptr };                          // Capture-IR, Shift-IR
    if (headBits > 0) {
        segs[n++] = { static_cast<uint8_t>(headBits), 0, 0, tdi };  // 前 total-1 位
    }
    segs[n++] = { 1, 1, 0, &lastByte };                        // 最后 1 位 + Exit1-IR（同一拍）
    segs[n++] = { 1, 1, 0, nullptr };                          // Update-IR
    segs[n++] = { 1, 0, 0, nullptr };                          // -> Run-Test/Idle

    if (ORBMDK::DAP_JTAG_Sequence(segs, n, nullptr, 0, nullptr) != 0) {
        LOG_WARN("JtagSetIr: IR=0x%02X 序列下发失败", ir);
        return false;
    }
    ctx->jtagCurIr = ir;
    return true;
}

// 一次 35(+旁路) 位 DR 扫描（IR 必须先选好）。返回 ack（线上编码）到 ackOut。
static bool JtagDrScan(RDDIContext* ctx, uint8_t request, uint32_t wdata,
                       uint32_t* rData, uint8_t* ackOut)
{
    const int idx = ctx->jtagDpIndex;
    const int before = idx;                                    // TDO 侧器件数（各 1 位 BYPASS）
    const int after  = ctx->jtagChainCount - idx - 1;           // TDI 侧器件数
    const int total  = 35 + before + after;
    if (total > 64) {
        LOG_ERROR("JtagDrScan: DR 位数 %d 超出单段上限 64（链太长）", total);
        return false;
    }

    uint8_t tdi[8] = {0};
    int pos = 0;
    pos += before;                                              // 旁路位（DR 仅 1 位，值随意）
    JtagPackBits(tdi, &pos, (request >> 1) & 1u, 1);            // RnW
    JtagPackBits(tdi, &pos, (request >> 2) & 1u, 1);            // A2
    JtagPackBits(tdi, &pos, (request >> 3) & 1u, 1);            // A3
    JtagPackBits(tdi, &pos, wdata, 32);                         // D0..D31
    pos += after;

    LOG_DEBUG("JtagDrScan: req=0x%02X wdata=0x%08X total=%d before(TDO侧)=%d after(TDI侧)=%d tdi=%s",
              request, wdata, total, before, after, JtagHex(tdi, (total + 7) / 8).c_str());

    // 同 JtagSetIr：Shift-DR -> Exit1-DR 的那一拍也要移位，故最后一拍必须 TMS=1。
    const int headBits = total - 1;
    const int lastBit  = (tdi[(total - 1) >> 3] >> ((total - 1) & 7)) & 1u;
    const uint8_t lastByte = static_cast<uint8_t>(lastBit);

    ORBMDK::JtagSeg segs[6];
    int n = 0;
    segs[n++] = { 1, 1, 0, nullptr };                          // Idle -> Select-DR
    segs[n++] = { 2, 0, 0, nullptr };                          // Capture-DR, Shift-DR
    if (headBits > 0) {
        segs[n++] = { static_cast<uint8_t>(headBits), 0, 1, tdi };   // 前 total-1 位 + 捕获
    }
    segs[n++] = { 1, 1, 1, &lastByte };                        // 最后 1 位 + Exit1-DR（同一拍）+ 捕获
    segs[n++] = { 1, 1, 0, nullptr };                          // Update-DR
    segs[n++] = { 1, 0, 0, nullptr };                          // -> Run-Test/Idle

    uint8_t tdo[8] = {0};
    if (ORBMDK::DAP_JTAG_Sequence(segs, n, tdo, sizeof(tdo), nullptr) != 0) return false;

    // 线上每个捕获段各自「按字节对齐」（补足 (bits+7)/8 字节），所以不能直接把
    // 各段字节当连续位流 —— 先按位拼回一条连续位流再解码。
    uint8_t stream[8] = {0};
    int sp = 0;
    if (headBits > 0) {
        for (int i = 0; i < headBits; ++i) {
            if ((tdo[i >> 3] >> (i & 7)) & 1u) {
                stream[sp >> 3] |= static_cast<uint8_t>(1u << (sp & 7));
            }
            ++sp;
        }
    }
    if (tdo[(headBits + 7) / 8] & 1u) {                        // 末段的 1 位
        stream[sp >> 3] |= static_cast<uint8_t>(1u << (sp & 7));
    }
    ++sp;

    int p = 0;
    p += before;                                                // 丢弃 TDO 侧旁路位
    const uint32_t c0 = JtagUnpackBits(stream, &p, 1);
    const uint32_t c1 = JtagUnpackBits(stream, &p, 1);
    const uint32_t c2 = JtagUnpackBits(stream, &p, 1);
    const uint32_t data32 = JtagUnpackBits(stream, &p, 32);
    const uint8_t  ack = static_cast<uint8_t>((c0 << 1) | (c1 << 0) | (c2 << 2));
    if (rData)  *rData  = data32;
    if (ackOut) *ackOut = ack;
    LOG_DEBUG("JtagDrScan: tdo=%s -> raw c0=%u c1=%u c2=%u ack=%u data=0x%08X (%s)",
              JtagHex(stream, (total + 7) / 8).c_str(), c0, c1, c2, ack, data32,
              ack == 1 ? "OK" : (ack == 2 ? "WAIT" : "NO_ACK/FAULT"));
    return true;
}

// ===========================================================================
// ⛔ 以下整段是**已作废**的「JTAG 位流批量 DR 写」实现，用 #if 0 保留供考古。
//
// 作废结论（2026-10-03，参考 orbtrace-1.4.3 门级 DAP 唯一权威源）：
//   WAIT 的语义是「这一笔**没完成**，必须原样重发同一笔」——见
//   `orbtrace/debug/cmsis_dap.py:712-716`（ACK_WAIT ⇒ 退回状态 6 重发同一笔
//   TRANSACT，waitRetry 默认 4096，:219）。`段ACK=[2,1,2,1,2,1]` 是**真 WAIT**，
//   不是 ACK 采样错位；逐笔路径同样 31% WAIT（`[single=20 wait=9]`）可证。
//   ⇒ 位流把 N 笔一次连发：返回 WAIT 的那半**被丢掉**，调用方又整体重写
//     ⇒ 丢字 + 写两遍 + AP TAR 多自增，三者叠加。
//   ⇒ **位流拼接式批量写在语义上不可能正确，没有"采样点"可修。**
//   合法批量只能交给「能重发的那一层」：固件门级 ID_DAP_TRANSFER（DAP_TransferMulti）。
//
// ⚠ 任何人想把它 #if 1 复活：先读完上面结论 —— 它解决不了 WAIT，只会再次静默错址。
// ===========================================================================
#if 0
// ---------------------------------------------------------------------------
// JTAG 批量 DR 写：把 N 笔「同一 AP 寄存器」的写合并进**一条** ID_DAP_JTAG_SEQUENCE(0x14)。
//
// 背景（2026-10-03 实测）：JTAG 会话下固件块传输被禁用（§18.9 第十步），
// `DAP_RegWriteRepeat` 逐笔往返 —— 实测 4.0 B/往返、170.7 us/往返、22.9 kB/s，
// 而 SWD 块传输是 455 B/往返、563 kB/s。往返数是唯一瓶颈，故把 N 笔塞进一条命令。
//
// ⚠ 关键教训（首版即错，自检 read[0]==pattern[N-1] 暴露）：DAP 的 DR 只有 35(+旁路)
//   位，**一段连续 Shift-DR 移 N×36 位 ≠ N 次访问** —— DAP 只在 Update-DR 处理 DR
//   内容，只有末尾那笔真正生效。每笔必须完整走
//   Select-DR → Capture-DR → Shift-DR → Exit1-DR → Update-DR 循环。
//   相邻循环可拼段（导航拍可共用），但**每笔仍须回到 Run-Test/Idle** —— 见下一段。
//
// ⚠ 隔字丢失的根因（2026-10-03 实测 mem[i]==pattern[2i] 的复盘）：最初的拼段是
//   Update-DR --TMS=1--> Select-DR 直达下一笔，**全程不进 Run-Test/Idle**。
//   而参考固件 JTAG_DP.c:JTAG_Transfer() 每次访问都以 Update-DR → TMS=0 → **Idle**
//   收尾（idle_cycles 也加在这里），本层的 JtagDrScan 同样以 {1,0,0} 落回 Idle。
//   JTAG-DP 的 AP 访问是 posted 的：上一笔尚未提交时发来的那一笔会被**静默丢掉**，
//   且应答仍是上一笔的 OK —— 所以既没有 WAIT 也没有 FAULT 可查，隔一笔丢一笔，
//   TAR 只随真正生效的那笔自增，表现为 mem[i]==pattern[2i]。故每笔之间必须补 Idle。
//
// 每笔的段（**与 JtagDrScan 逐段等价**，导航拍单独成段且不捕获）：
//   {2,  TMS=0, 不捕获}     Select-DR→Capture-DR→Shift-DR（导航 2 拍，不移位、不捕获）
//   {total-1, TMS=0, 捕获}  首 total-1 个移位拍（捕获）
//   {2,  TMS=1, 不捕获}     末位移位拍（Shift-DR→Exit1-DR）+ Exit1-DR→Update-DR，共 2 拍
//   {1,  TMS=0, 不捕获}     Update-DR → Run-Test/Idle（提交本笔的 posted AP 访问）
//   {1,  TMS=1, 不捕获}     Run-Test/Idle → Select-DR 接下一笔（末笔不补，直接以 Idle 收尾）
// 移位拍合计 = (total-1) + 1 = total，恰好填满 total 位 DR。
// ⚠ 2026-10-03 修正（首版把导航 2 拍并入捕获段、采样按 2+before 跳过 → 段ACK 恒 2,1 交替）：
//   固件对捕获段是「按段字节对齐」返回，但**导航拍并进捕获段**后采样点与 posted 应答错位，
//   首笔即误判 WAIT。改为与 JtagDrScan 完全一致的分段（导航拍不捕获），采样点回到 before。
// 包长：请求 12K+3 B（2 头 + 每笔 nav2/cap6/last2/update1/link1 + 首个 Idle→Select 1）、
//       响应 3+5K B ⇒ V2/Bulk(508B) K≤42（保守取 41）；V1/HID(64B) K≤4。
//
// ACK：JTAG-DP 是 posted 语义 —— 第 k 笔扫描的捕获是**第 k-1 笔**访问的应答
// （JtagDapTransfer 的 RDBUFF 取数同理）。故此处校验写 0..K-2（捕获 1..K-1），
// 末笔的 ACK 由随后的 JtagDapFlush（RDBUFF 读）兜底。任何一笔非 OK 即返回，
// 调用方回退逐笔 —— 批量只影响速度，不改变正确性判定。
// ---------------------------------------------------------------------------
static constexpr int kJtagBurstMaxBulk = 41;   // V2/Bulk：请求 12*41+3=495 B / 响应 3+5*41=208 B
static constexpr int kJtagBurstMaxHid  = 4;    // V1/HID ：请求 12*4+3=51 B / 响应 3+5*4=23 B（+报告ID 1 B ≤ 64）

static int JtagBurstWordsLimit()
{
    return (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) ? kJtagBurstMaxBulk
                                                            : kJtagBurstMaxHid;
}

// 一条命令写入 count 笔（count >= 2）。返回 ORBMDK::DAP_RES_*
//
// ⛔ **已停用（2026-10-03）**：默认不调用 —— 见 RDDIContext::jtagApBurstEnabled 的完整说明。
//    核心缺陷：本函数把 count 笔位流**一次发完**（写已提交）之后才事后校验 ACK，而调用方
//    把"返回失败"理解为"一笔都没写"⇒ 整体重写 ⇒ 数据被写两遍、AP TAR 多自增一段 ⇒ 静默错址。
//    实证：`第 0/7 笔 ack=2（WAIT）-> 回退逐笔` + `段ACK=[2,1,2,1,2,1]`（错位误判，非真 WAIT）。
static int JtagApWriteBurst(RDDIContext* ctx, uint8_t request, int count, const int* data)
{
    if (count < 2) return ORBMDK::DAP_RES_NO_ACK;
    if (count > kJtagBurstMaxBulk) count = kJtagBurstMaxBulk;

    const int idx    = ctx->jtagDpIndex;
    const int before = idx;                                   // TDO 侧器件数（各 1 位旁路）
    const int after  = ctx->jtagChainCount - idx - 1;          // TDI 侧器件数
    const int total  = 35 + before + after;                    // 本链 = 36
    const int capBits = total - 1;                             // 捕获段的移位拍数（末位交给 {2,1,0} 段）
    if (capBits > 63) {                                        // info 只有 6 位（0 表 64）
        LOG_WARN("JtagApWriteBurst: DR 单段 %d 位超出 63，放弃批量", capBits);
        return ORBMDK::DAP_RES_NO_ACK;
    }

    const uint8_t ir = (request & 0x01) ? kJtagIrApacc : kJtagIrDpacc;
    if (!JtagSetIr(ctx, ir)) return ORBMDK::DAP_RES_NO_ACK;

    const int headBytes = (capBits + 7) / 8;                   // 5
    ORBMDK::JtagSeg segs[5 * kJtagBurstMaxBulk + 2];           // 每笔 5 段（导航/捕获/末2拍/提交/链接）
    // 按 headBytes 上限（8 = 63 位）开，链长时 headBytes>5 也不会越界
    uint8_t tdiStore[kJtagBurstMaxBulk * 8] = {0};
    uint8_t lastBits[kJtagBurstMaxBulk]     = {0};
    uint8_t tdoStore[kJtagBurstMaxBulk * 8] = {0};

    int n = 0;
    segs[n++] = { 1, 1, 0, nullptr };                          // Idle -> Select-DR
    for (int i = 0; i < count; ++i) {
        // 本笔 total 位请求流（位序与 JtagDrScan 完全一致）
        uint8_t s[8] = {0};
        int pos = 0;
        pos += before;                                         // TDO 侧旁路位
        JtagPackBits(s, &pos, (request >> 1) & 1u, 1);         // RnW = 0（写）
        JtagPackBits(s, &pos, (request >> 2) & 1u, 1);         // A2
        JtagPackBits(s, &pos, (request >> 3) & 1u, 1);         // A3
        JtagPackBits(s, &pos, static_cast<uint32_t>(data[i]), 32);
        pos += after;                                          // TDI 侧旁路位

        // 捕获段从第一个移位拍开始（导航 2 拍独立成段、不捕获），故请求位从位 0 起放；
        // 最后一个移位拍交给 {2,TMS=1} 段。
        uint8_t* t = &tdiStore[i * headBytes];
        for (int b = 0; b < total - 1; ++b) {
            if ((s[b >> 3] >> (b & 7)) & 1u) {
                t[b >> 3] |= static_cast<uint8_t>(1u << (b & 7));
            }
        }
        lastBits[i] = static_cast<uint8_t>((s[(total - 1) >> 3] >> ((total - 1) & 7)) & 1u);

        segs[n++] = { 2, 0, 0, nullptr };                          // 导航：Select-DR→Capture-DR→Shift-DR（不捕获）
        segs[n++] = { static_cast<uint8_t>(capBits), 0, 1, t };    // 前 total-1 个移位拍（捕获）
        segs[n++] = { 2, 1, 0, &lastBits[i] };                     // 末位移位 + Exit1-DR + Update-DR（同拍）
        segs[n++] = { 1, 0, 0, nullptr };                          // Update-DR -> Run-Test/Idle（提交本笔）
        if (i != count - 1) {
            segs[n++] = { 1, 1, 0, nullptr };                      // Idle -> Select-DR 接下一笔
        }
    }

    size_t tdoLen = 0;
    if (ORBMDK::DAP_JTAG_Sequence(segs, n, tdoStore, sizeof(tdoStore), &tdoLen) != 0) {
        return ORBMDK::DAP_RES_NO_ACK;
    }
    if (tdoLen < static_cast<size_t>(count * headBytes)) {
        LOG_WARN("JtagApWriteBurst: TDO 只回 %llu 字节（需要 %d）",
                 (unsigned long long)tdoLen, count * headBytes);
        return ORBMDK::DAP_RES_NO_ACK;
    }

    // posted ACK：第 i 段捕获（跳过 before 个旁路位后 3 位）= 第 i-1 笔写的应答
    LOG_DEBUG("JtagApWriteBurst: tdoLen=%llu cap[0]=%s cap[1]=%s cap[2]=%s (headBytes=%d)",
              (unsigned long long)tdoLen,
              JtagHex(tdoStore, headBytes).c_str(),
              JtagHex(tdoStore + headBytes, headBytes).c_str(),
              JtagHex(tdoStore + 2 * headBytes, headBytes).c_str(), headBytes);
    for (int i = 1; i < count; ++i) {
        const uint8_t* cap = &tdoStore[i * headBytes];
        const int p0 = before;                                     // 捕获段从首个移位拍起（无导航偏移）
        const uint32_t c0 = (cap[p0 >> 3] >> (p0 & 7)) & 1u;
        const uint32_t c1 = (cap[(p0 + 1) >> 3] >> ((p0 + 1) & 7)) & 1u;
        const uint32_t c2 = (cap[(p0 + 2) >> 3] >> ((p0 + 2) & 7)) & 1u;
        const uint8_t  ack = static_cast<uint8_t>((c0 << 1) | (c1 << 0) | (c2 << 2));
        if (ack != 1) {
            // 把整条 burst 每段的 ACK 都解析出来（段 i 的捕获 = 第 i-1 笔的应答，见上），
            // 便于判断"从第几笔开始异常 / 是否整体移位错位 / 是否只末笔缺应答"。
            char acksSeq[4 * kJtagBurstMaxBulk + 4];
            int  ap = 0;
            acksSeq[ap++] = '[';
            for (int k = 1; k < count && ap < static_cast<int>(sizeof(acksSeq)) - 3; ++k) {
                const uint8_t* c = &tdoStore[k * headBytes];
                const uint32_t b0 = (c[p0 >> 3] >> (p0 & 7)) & 1u;
                const uint32_t b1 = (c[(p0 + 1) >> 3] >> ((p0 + 1) & 7)) & 1u;
                const uint32_t b2 = (c[(p0 + 2) >> 3] >> ((p0 + 2) & 7)) & 1u;
                const uint8_t  a  = static_cast<uint8_t>((b0 << 1) | (b1 << 0) | (b2 << 2));
                acksSeq[ap++] = static_cast<char>('0' + ((a <= 7) ? a : 7));
                if (k != count - 1) acksSeq[ap++] = ',';
            }
            acksSeq[ap]     = ']';
            acksSeq[ap + 1] = '\0';
            LOG_WARN("JtagApWriteBurst: 第 %d/%d 笔 ack=%u（%s）-> 回退逐笔"
                     " | req=0x%02X ir=%u(%s) RnW=%u A2=%u A3=%u data=0x%08X"
                     " | cap[段%d]=%s c=%u%u%u"
                     " | chain=%d before=%d after=%d total=%d capBits=%d headBytes=%d tdoLen=%llu"
                     " | 已通过=%d 段ACK(段i=第i-1笔)=%s",
                     i - 1, count, ack, (ack == 2) ? "WAIT" : "FAULT/NO_ACK",
                     request, ir, (ir == kJtagIrApacc) ? "APACC" : "DPACC",
                     (request >> 1) & 1u, (request >> 2) & 1u, (request >> 3) & 1u,
                     static_cast<uint32_t>(data[i - 1]),
                     i, JtagHex(cap, headBytes).c_str(), c0, c1, c2,
                     ctx->jtagChainCount, before, after, total, capBits, headBytes,
                     (unsigned long long)tdoLen,
                     i - 1, acksSeq);
            return JtagAckToRes(ack);
        }
    }

    ctx->jtagWritePending = true;      // 末笔的提交交给下一次访问 / JtagDapFlush（兜底其 ACK）
    LOG_DEBUG("JtagApWriteBurst: 一条命令写入 %d 字（request=0x%02X, %d 段, %d 位/笔）",
              count, request, n, total);
    return ORBMDK::DAP_RES_OK;
}
#endif   // ⛔ 已作废的位流批量写 —— 勿 #if 1，见上方结论（WAIT 必须重发同一笔）

// 单次 DAP 传输（JTAG）。request 位定义与 SWD 一致：
//   bit0 = APnDP, bit1 = RnW, bit2 = A2, bit3 = A3（bit4.. 的 MATCH/TIMESTAMP 未实现）
// 返回值语义与 ORBMDK::DAP_Transfer 对齐（ORBMDK::DAP_RES_*）。
static int JtagDapTransfer(RDDIContext* ctx, int request, uint32_t* data)
{
    const uint8_t req = static_cast<uint8_t>(request);
    const bool isRead = (req & 0x02) != 0;
    const uint8_t ir = (req & 0x01) ? kJtagIrApacc : kJtagIrDpacc;

    if (!JtagSetIr(ctx, ir)) return ORBMDK::DAP_RES_NO_ACK;

    // WAIT 重试：AP 传输常需等上一拍完成（等价于固件的 transfer.retry_count）
    uint8_t ack = 0;
    uint32_t rData = 0;
    int retry = (kDefaultWaitRetry > 0) ? kDefaultWaitRetry : 1;
    do {
        if (!JtagDrScan(ctx, req, isRead ? 0u : (data ? *data : 0u), &rData, &ack)) {
            return ORBMDK::DAP_RES_NO_ACK;
        }
    } while (ack == 2 && --retry > 0);
    if (ack != 1) return JtagAckToRes(ack);

    if (!isRead) {
        ctx->jtagWritePending = true;   // 写已进 DP 流水线：由下一次访问（或 Flush）提交
        return ORBMDK::DAP_RES_OK;
    }

    // posted read：本次扫描只发出请求，数据要在**下一次**访问返回 ——
    // 读 DP_RDBUFF 取回（DAP.c DAP_JTAG_Transfer 同款；RDBUFF 是 DP 寄存器，IR 必须切 DPACC）
    if (!JtagSetIr(ctx, kJtagIrDpacc)) return ORBMDK::DAP_RES_NO_ACK;
    ctx->jtagWritePending = false;      // 上面那次访问已把挂起的写提交掉
    retry = (kDefaultWaitRetry > 0) ? kDefaultWaitRetry : 1;
    do {
        if (!JtagDrScan(ctx, 0x0E /* DP_RDBUFF | RnW */, 0, &rData, &ack)) {
            return ORBMDK::DAP_RES_NO_ACK;
        }
    } while (ack == 2 && --retry > 0);
    if (ack != 1) return JtagAckToRes(ack);

    if (data) *data = rData;
    return ORBMDK::DAP_RES_OK;
}

// 提交挂起的写：ADIv5 JTAG-DP 的写和读一样"下一拍生效"，孤立的一次写需要
// 一次后续访问才会真正落到目标上（与固件 DAP.c 里 "Check last write" 的 RDBUFF 读等价）。
static bool JtagDapFlush(RDDIContext* ctx)
{
    if (ctx->isSWD || !ctx->jtagWritePending) return true;
    if (!JtagSetIr(ctx, kJtagIrDpacc)) return false;

    uint8_t ack = 0;
    uint32_t dummy = 0;
    int retry = (kDefaultWaitRetry > 0) ? kDefaultWaitRetry : 1;
    do {
        if (!JtagDrScan(ctx, 0x0E /* DP_RDBUFF | RnW */, 0, &dummy, &ack)) return false;
    } while (ack == 2 && --retry > 0);

    ctx->jtagWritePending = false;
    return ack == 1;
}

// 统一的传输入口：SWD 走固件命令，JTAG 走本层引擎
static int DapTransferFor(RDDIContext* ctx, int dapId, uint8_t request, uint32_t* data)
{
    if (ctx->isSWD) return ORBMDK::DAP_Transfer(dapId, request, data);
    return JtagDapTransfer(ctx, request, data);
}

// ---------------------------------------------------------------------------
// JTAG 建链（COMPAT_ANALYSIS §18.9 落地）
//
// 与 SWD 的关键差别：JTAG 的 IR/DR 时序**全部由本层生成**（见上方引擎），
// 固件只负责把位流打到线上。顺序：
//     1) DAP_Connect(port=JTAG)        确认探针真的进了 JTAG 模式
//     2) SWD -> JTAG 切换序列          尽力而为（本目标为单向，通常需目标重新上电）
//     3) 扫链                          复位 TAP + 移出 IDCODE，自动得出 IR 长度表与 DP 位置
//     4) DAP_JTAG_Configure            把链布局同步给固件（可选，本层不依赖）
//     5) 读 DP IDCODE                  选 IR -> 发读请求 -> 读 RDBUFF（posted read）
// ---------------------------------------------------------------------------
static bool JtagInitSequence(RDDIContext* ctx, int* outMode, uint32_t* outIdcode)
{
    if (outMode)   *outMode = 0;
    if (outIdcode) *outIdcode = 0;

    const int mode = ORBMDK::DAP_ConnectTargetPort(DAP_CONNECT_JTAG);
    if (outMode) *outMode = mode;
    if (mode != DAP_CONNECT_JTAG) {
        LOG_ERROR("JtagInitSequence: DAP_Connect(port=JTAG) -> mode=%d（探针未进入 JTAG）", mode);
        return false;
    }
    LOG_INFO("JtagInitSequence: 探针已进入 JTAG 模式");

    // SWD -> JTAG 切换序列（ADIv5 / OpenOCD 的 swd_seq_swd_to_jtag）：
    // ① TMS/SWDIO 保持 1 连续 ≥50 拍（线复位）② 在 TMS/SWDIO 上发 16 位 0xE73C。
    // SWJ-DP 有"模式记忆"，刚用 SWD 通信过它就停在 SWD 模式，此时直接扫链必然为空。
    //
    // ⚠️ 实测（2026-09-30）：本目标（STM32 类 SWJ-DP）的 JTAG<-SWD 是**单向**的 ——
    // 上电后只要跑过一次 SWD，补发这个序列也回不到 JTAG，**必须给目标断电重启**。
    // 所以这里只作"协议正确"的尽力而为，失败不再当硬件故障报（见下面的日志提示）。
    {
        uint8_t lineReset[8];
        memset(lineReset, 0xFF, sizeof(lineReset));      // 64 拍全 1
        ORBMDK::DAP_SWJ_Sequence(64, lineReset);
        const uint8_t swdToJtag[2] = { 0x3C, 0xE7 };     // 0xE73C, LSB first
        ORBMDK::DAP_SWJ_Sequence(16, swdToJtag);

        // SWJ 序列会把探针切到"引脚直驱"模式，切完要重新选一次 JTAG
        const int mode2 = ORBMDK::DAP_ConnectTargetPort(DAP_CONNECT_JTAG);
        if (mode2 != DAP_CONNECT_JTAG) {
            LOG_WARN("JtagInitSequence: 切换序列后重新选 JTAG 返回 mode=%d", mode2);
        }
    }

    // 扫链：复位 TAP + 移出 IDCODE，全走通用 JTAG 位流（本层引擎）
    uint32_t ids[8] = {0};
    const int devCount = JtagScanChain(ids, 8);
    if (devCount <= 0) {
        LOG_ERROR("JtagInitSequence: 扫链为空 —— TDI/TDO 上没有器件响应。"
                  "若刚刚用过 SWD：本目标 SWJ-DP 的 JTAG<-SWD 是单向的，"
                  "**必须给目标断电重启**后再试（实测结论，不是接线问题）。"
                  "其它检查：① TDI/TDO 两根线是否接好（SWD 只用 4 根）；"
                  "② JTAG 引脚是否被复用或被选项字节关闭（部分 STM32 可配成 JTAG-DP Disabled）；"
                  "③ 目标是否在复位/掉电。详见 COMPAT_ANALYSIS §18.9。");
        return false;
    }

    // 配链：ir_length[0] = 离 TDO 最近的器件（与固件 JTAG_DP.c 的约定一致）。
    // DP 的位置由 IDCODE 自动判定，不依赖 OpenOCD 的声明顺序。
    int dpIndex = -1;
    for (int i = 0; i < devCount && i < 8; ++i) {
        int irLen = JtagIrLenForId(ids[i]);
        if (irLen == 0) {
            LOG_WARN("JtagInitSequence: IDCODE 0x%08X 未知，IR 长度按 4 位兜底", ids[i]);
            irLen = 4;
        }
        ctx->jtagIrLens[i] = static_cast<uint8_t>(irLen);
        if (dpIndex < 0 && (ids[i] & 0x0FFFFFFFu) == 0x0BA00477u) dpIndex = i;
    }
    for (int i = devCount; i < 8; ++i) ctx->jtagIrLens[i] = 0;
    if (dpIndex < 0) {
        LOG_WARN("JtagInitSequence: 链上没找到 ARM JTAG-DP(0x?BA00477)，暂按位置 0 处理");
        dpIndex = 0;
    }

    ctx->jtagChainCount   = devCount;
    ctx->jtagDpIndex      = dpIndex;
    ctx->jtagDevCount     = devCount;
    ctx->jtagIrLength     = ctx->jtagIrLens[dpIndex];
    ctx->jtagChainIrBits  = 0;
    for (int i = 0; i < devCount; ++i) ctx->jtagChainIrBits += ctx->jtagIrLens[i];
    // 链上每个 TAP 的 IDCODE 留档（AGDI 的器件列表按它画，见 ctx->jtagIds 的说明）
    for (int i = 0; i < 8; ++i) {
        ctx->jtagIds[i] = (i < devCount) ? ids[i] : 0;
    }
    ctx->jtagCurIr        = 0;       // 未知：让首次访问自己选 IR
    ctx->jtagWritePending = false;

    LOG_INFO("JtagInitSequence: 扫链成功 count=%d DP@%d(IR=%d) 全链IR=%d位  ID[0]=0x%08X ID[1]=0x%08X",
             devCount, dpIndex, static_cast<int>(ctx->jtagIrLength), ctx->jtagChainIrBits,
             ids[0], ids[1]);

    // 把链布局同步给固件（供其 JTAG_IDCODE 等高层命令使用；本层引擎不依赖它）。
    // ⚠ 固件侧是硬编码位宽：irlenx = 6 x 5 bit、dev/ndevs = 3 bit
    //   ⇒ 超过 6 个器件、或单器件 IR > 31 bit 时**不下发**（硬发会被静默截断，
    //     走链错会让通道静默变哑）。本层引擎走 JTAG_SEQUENCE，不依赖它。
    {
        uint8_t irLens[8] = {0};
        for (int i = 0; i < devCount && i < 8; ++i) irLens[i] = ctx->jtagIrLens[i];
        if (!JtagChainFitsFirmware(devCount, irLens)) {
            LOG_WARN("JtagInitSequence: 链布局超出固件容量（器件数=%d > %d 或某器件 IR > %d bit）"
                     "-> 跳过 DAP_JTAG_Configure（本层引擎不依赖固件链布局）",
                     devCount, kJtagFwMaxDevices, kJtagFwMaxIrBits);
        } else if (ORBMDK::DAP_JTAG_Configure(irLens, static_cast<uint8_t>(devCount)) != 0) {
            LOG_WARN("JtagInitSequence: DAP_JTAG_Configure 下发失败（本层引擎不依赖）");
        }
    }

    ORBMDK::DAP_ConfigureTransfer(0, kDefaultWaitRetry, kDefaultMatchRetry);

    // 读 DP IDCODE：
    //   ① 扫链（IDCODE 指令路径）已拿到 DP 的 IEEE1149.1 IDCODE（ids[dpIndex]）；
    //   ② 再补一次 DPACC 读 DPIDR(0x0) 作为调试通路校验。
    //
    // 实测结论（对照 OpenOCD，2026-09-30）：本目标 STM32 的 ARM JTAG-DP 对 **DPACC
    // 地址 0x0 的读恒返回 0**（同一条 DPACC 通道读 CTRL/STAT(0x4)=0xF0000000、经
    // APACC 读 AP IDR(0xFC)=0x24770011 均正确）。OpenOCD 的 dap_dp_init() 同样
    // **从不**读 DPIDR：它只 poll CTRL/STAT(0x4) 等 PWRUPACK，再读 AP IDR 识别 MEM-AP。
    // 因此这里把建链判据从「DPIDR 值非 0」改为「DPIDR 读的 ACK 正常」，DP 身份优先用
    // 扫链 IDCODE，DPIDR 仅在其可读时覆盖。
    uint32_t dpidr = 0;
    const int st = JtagDapTransfer(ctx, 0x02, &dpidr);
    LOG_INFO("JtagInitSequence: DPIDR(DPACC 0x0) 读 -> status=%d, dpidr=0x%08X", st, dpidr);

    const uint32_t chainId = (dpIndex >= 0 && dpIndex < devCount) ? ids[dpIndex] : 0;
    if (st != ORBMDK::DAP_RES_OK) {
        LOG_ERROR("JtagInitSequence: DP 无应答（status=%d）；扫链 IDCODE=0x%08X", st, chainId);
        return false;
    }
    if (dpidr == 0 || dpidr == 0xFFFFFFFFu) {
        LOG_INFO("JtagInitSequence: DPACC 0x0 读回 0x%08X（本 DP 不支持读 DPIDR），"
                 "改用扫链 IDCODE=0x%08X 作为 DP 身份", dpidr, chainId);
    }
    const uint32_t idcode = (dpidr != 0 && dpidr != 0xFFFFFFFFu) ? dpidr : chainId;
    if (outIdcode && *outIdcode == 0) *outIdcode = idcode;

    // 清 sticky：ABORT(0x1E) = IR=ABORT 的 DR 写（与固件 JTAG_WriteAbort 同款）
    {
        uint8_t ack = 0;
        uint32_t dummy = 0;
        if (JtagSetIr(ctx, kJtagIrAbort) &&
            JtagDrScan(ctx, 0x00 /* RnW=A2=A3=0 */, 0x1Eu, &dummy, &ack)) {
            ctx->jtagWritePending = true;   // ABORT 同样走流水线：补一次访问提交
            JtagDapFlush(ctx);
            LOG_DEBUG("JtagInitSequence: DP ABORT(0x1E) 已下发 ack=%d", ack);
        }
        ctx->jtagCurIr = 0;   // IR 已被 ABORT 改过，标记为未知
    }

    ctx->jtagRecoverFails = 0;   // 建链成功：清空自愈失败计数
    return true;
}

static bool SwdLinkRecover(RDDIContext* ctx)
{
    if (ctx->swdRecovering) {
        return false;
    }
    // JTAG 模式下**绝不能**走下面的 SWD 序列（线复位 + JTAG-to-SWD 会把刚建好的
    // JTAG 链路直接打掉）：改为重新做一次 JTAG 建链（§18.9）。
    if (!ctx->isSWD) {
        ctx->swdRecovering = true;
        // 风暴保护：JTAG 建链本身若连续失败（例如目标在 SWD 模式），每次失败都重初始化会
        // 把日志刷爆、并让探针长时间满负荷。连续失败到阈值后只汇报、不再重初始化。
        if (ctx->jtagRecoverFails >= kJtagRecoverMaxFails) {
            ctx->swdRecovering = false;
            LOG_ERROR("LinkRecover: JTAG 链路已连续 %d 次建链失败，停止重试。"
                      "请检查目标是否需断电重启/线序，修复后再重新连接。",
                      ctx->jtagRecoverFails);
            return false;
        }
        LOG_WARN("LinkRecover: JTAG 模式 -> 重新初始化 JTAG 链路（第 %d/%d 次）",
                 ctx->jtagRecoverFails + 1, kJtagRecoverMaxFails);
        int mode = 0;
        uint32_t idcode = 0;
        const bool ok = JtagInitSequence(ctx, &mode, &idcode);
        ctx->jtagRecoverFails = ok ? 0 : (ctx->jtagRecoverFails + 1);
        ctx->swdRecovering = false;
        return ok;
    }

    ctx->swdRecovering = true;

    uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    uint8_t jtagToSwd[] = { 0x9E, 0xE7 };
    uint8_t idle[] = { 0x00 };

    bool ok = false;
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (attempt > 0) {
            // 第二轮先重新 DAP_Connect(port=SWD)，再走完整切换序列
            ORBMDK::DAP_ConnectTarget();
        }
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(16, jtagToSwd);
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(8, idle);

        // ABORT 走专用命令(0x08)：即使 DAP_Transfer 全 NO_ACK，它也通常能下发
        ORBMDK::DAP_WriteAbort(0, 0x1E);   // ORUNERRCLR|WDERRCLR|STKERRCLR|STKCMPCLR

        uint32_t idcode = 0;
        if (ORBMDK::DAP_Transfer(0, 0x02, &idcode) == ORBMDK::DAP_RES_OK &&
            idcode != 0 && idcode != 0xFFFFFFFFu) {
            ok = true;
            LOG_WARN("SWD link recovered: idcode=0x%08X (attempt=%d)", idcode, attempt + 1);
        }
    }

    ctx->swdRecovering = false;
    if (!ok) {
        LOG_ERROR("SWD link recovery failed: target still not responding");
    }
    return ok;
}

// 带自愈的传输：NO_ACK / FAULT 时恢复链路并重放一次（JTAG 走本层引擎）
static int TransferWithRecovery(RDDIContext* ctx, int dapId, uint8_t request, uint32_t* data)
{
    int status = DapTransferFor(ctx, dapId, request, data);
    if ((status == ORBMDK::DAP_RES_NO_ACK || status == ORBMDK::DAP_RES_FAULT) &&
        !ctx->swdRecovering && SwdLinkRecover(ctx)) {
        status = DapTransferFor(ctx, dapId, request, data);
    }
    return status;
}

RDDI_FUNC int DAP_ReadReg(const RDDIHandle handle, const int dapId, const int regId, int *value)
{
    LOG_DEBUG("DAP_ReadReg: enter, handle=%d, dapId=%d, regId=0x%08X", handle, dapId, regId);
    if (value) {
        *value = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("DAP_ReadReg: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    if (!value) {
        return RDDI_BADARG;
    }

    const int regOffset = GetRegOffset(regId);
    if (regOffset < 0) {
        LOG_ERROR("DAP_ReadReg: unsupported regID=0x%08X", regId);
        return RDDI_DAP_BAD_REGISTER_ID;
    }

    // 注意：AP bank 由调用方通过 DAP_WriteReg(DP_SELECT) 管理，本函数**不得**
    // 改写 DP SELECT —— 否则会覆盖 AGDI 设置的 APSEL/APBANKSEL。
    const uint8_t request = static_cast<uint8_t>(regOffset) | 0x02;  // Read

    uint32_t data = 0;
    int status = TransferWithRecovery(ctx, dapId, request, &data);

    if (status != ORBMDK::DAP_RES_OK) {
        ctx->lastError = RDDI_DAP_ERROR;
        ctx->lastErrorStr = "Transfer failed";
        // AGDI 的 SWD_CheckStatus/JTAG_CheckStatus 按数值识别这两个码并做恢复：
        //   0x100F -> 清 sticky 错误位后重试
        //   0x100E -> 写 ABORT 后重试
        // FAULT / WAIT / NO_ACK 都是**可恢复**的瞬时状态：AGDI 的 SWD_CheckStatus
        // 会识别 RDDI_DAP_DP_STICKY_ERR / RDDI_DAP_OPERATION_TIMEOUT，并做
        // "清 sticky 错误位"或"写 ABORT"后重试。因此按 WARN 记录，避免正常调试
        // 期间持续刷 ERROR 而掩盖真正的问题。
        if (status == ORBMDK::DAP_RES_FAULT) {
            LOG_WARN("DAP_ReadReg: FAULT, regId=0x%08X -> RDDI_DAP_DP_STICKY_ERR", regId);
            return RDDI_DAP_DP_STICKY_ERR;
        }
        if (status == ORBMDK::DAP_RES_WAIT || status == ORBMDK::DAP_RES_NO_ACK) {
            LOG_WARN("DAP_ReadReg: WAIT/NO_ACK, regId=0x%08X -> RDDI_DAP_OPERATION_TIMEOUT", regId);
            return RDDI_DAP_OPERATION_TIMEOUT;
        }
        LOG_ERROR("DAP_ReadReg: transfer failed, status=%d, regId=0x%08X", status, regId);
        return RDDI_DAP_ERROR;
    }

    *value = static_cast<int>(data);
    // ★ raw 返回值（**DEBUG** 级）：与 HID 层 "DAP_Transfer raw" 配对看 —— 前者是设备原始响应，
    //   这里是本层解析出的寄存器值，两条对齐即可排除"解析错"。
    LOG_DEBUG("DAP_ReadReg raw: dapId=%d, regId=0x%08X, request=0x%02X -> status=OK, value=0x%08X",
             dapId, regId, request, static_cast<unsigned>(data));
    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_WriteReg(const RDDIHandle handle, const int dapId, const int regId, const int value)
{
    LOG_DEBUG("DAP_WriteReg: enter, handle=%d, dapId=%d, regId=0x%08X, value=0x%08X",
              handle, dapId, regId, (unsigned)value);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("DAP_WriteReg: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    const int id = GetRegId(regId);

    // DP ABORT：ARM 中既可写编号 0（DP 地址 0x00 的写口），也可用专用编号 8
    if ((id == 0 || id == 8) && (regId & DAP_REG_RnW) == 0) {
        LOG_DEBUG("DAP_WriteReg: ABORT (id=%d), value=0x%08X", id, value);
        if (!ctx->isSWD) {
            // JTAG：IR=ABORT(0x08) 的 DR 写（RnW=A2=A3=0 + 32 位数据），ACK 忽略
            uint8_t ack = 0;
            uint32_t dummy = 0;
            const bool ok = JtagSetIr(ctx, kJtagIrAbort) &&
                            JtagDrScan(ctx, 0x00, static_cast<uint32_t>(value), &dummy, &ack);
            ctx->jtagCurIr = 0;   // IR 已被改，标记为未知
            if (ok) {
                ctx->jtagWritePending = true;   // ABORT 也走流水线：补一次访问提交
                JtagDapFlush(ctx);
            }
            return ok ? RDDI_SUCCESS : RDDI_DAP_ERROR;
        }
        return ORBMDK::DAP_WriteAbort(dapId, static_cast<uint32_t>(value)) == 0
                   ? RDDI_SUCCESS
                   : RDDI_DAP_ERROR;
    }

    const int regOffset = GetRegOffset(regId);
    if (regOffset < 0) {
        LOG_ERROR("DAP_WriteReg: unsupported regID=0x%08X", regId);
        return RDDI_DAP_BAD_REGISTER_ID;
    }

    // AP bank 由调用方通过 DAP_WriteReg(DP_SELECT) 管理，此处不改写 DP SELECT
    const uint8_t request = static_cast<uint8_t>(regOffset);  // Write

    uint32_t data = static_cast<uint32_t>(value);
    int status = TransferWithRecovery(ctx, dapId, request, &data);

    if (status != ORBMDK::DAP_RES_OK) {
        ctx->lastError = RDDI_DAP_ERROR;
        ctx->lastErrorStr = "Transfer failed";
        // 同 DAP_ReadReg：这两类是 AGDI 会自行恢复的瞬时状态，按 WARN 记录。
        // 写失败时 value 是排查关键信息，一并输出。
        if (status == ORBMDK::DAP_RES_FAULT) {
            LOG_WARN("DAP_WriteReg: FAULT, regId=0x%08X, value=0x%08X -> RDDI_DAP_DP_STICKY_ERR",
                     regId, (unsigned)value);
            return RDDI_DAP_DP_STICKY_ERR;
        }
        if (status == ORBMDK::DAP_RES_WAIT || status == ORBMDK::DAP_RES_NO_ACK) {
            LOG_WARN("DAP_WriteReg: WAIT/NO_ACK, regId=0x%08X, value=0x%08X -> RDDI_DAP_OPERATION_TIMEOUT",
                     regId, (unsigned)value);
            return RDDI_DAP_OPERATION_TIMEOUT;
        }
        LOG_ERROR("DAP_WriteReg: transfer failed, status=%d, regId=0x%08X", status, regId);
        return RDDI_DAP_ERROR;
    }

    // ★ raw 返回值（**DEBUG** 级）：写方向设备只回 ack/status，"到底写了什么"必须本地留痕
    LOG_DEBUG("DAP_WriteReg raw: dapId=%d, regId=0x%08X, request=0x%02X, value=0x%08X -> status=OK",
             dapId, regId, request, static_cast<unsigned>(value));

    // 状态 LED：跟踪 AP TAR(5) / DRW(7) 写入（官方 AGDI 不调用 DAP_HostStatus）
    if (id == 5) {          // DAP_REG_AP_0x4 = TAR
        TrackApTarWrite(ctx, static_cast<uint32_t>(value));
    } else if (id == 7) {   // DAP_REG_AP_0xC = DRW
        TrackApDataWrite(ctx, static_cast<uint32_t>(value));
    }

    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// 多笔传输（D2）辅助
// ---------------------------------------------------------------------------

/// DAP_RES_* → RDDI 错误码（逐笔与批量处理共用，保证两条路径的返回码一致）
static int MapDapStatusToRddi(int status)
{
    if (status == ORBMDK::DAP_RES_FAULT) {
        return RDDI_DAP_DP_STICKY_ERR;
    }
    if (status == ORBMDK::DAP_RES_WAIT || status == ORBMDK::DAP_RES_NO_ACK) {
        return RDDI_DAP_OPERATION_TIMEOUT;
    }
    return RDDI_INTERNAL_ERROR;
}

/// 一次成功写之后的副作用：状态 LED 跟踪 AP TAR(5) / DRW(7)。
/// AGDI 通过 SWD_WriteData（TAR + DRW）写 DHCSR，即走这条路径。
static void TrackLedForWrite(RDDIContext* ctx, int id, uint32_t data)
{
    if (id == 5) {          // TAR
        TrackApTarWrite(ctx, data);
    } else if (id == 7) {   // DRW
        TrackApDataWrite(ctx, data);
    }
}

// 多笔传输能力探测：**连读两次 DP IDCODE**（同一地址、同一值）并与单笔结果交叉比对
// —— 三者一致才认为固件正确实现了 count>1。
//
// 为什么不直接信"规范要求支持"：这条命令在 AGDI 的内存/寄存器热路径上，一旦固件
// 把 count 解释成别的意思（或只执行第一笔就返回），读回的数据会**静默错**（命令全报
// OK、值却是错的）。探测成本：2 次往返，每会话一次。
//
// ⚠ 这里**只**验证读语义。曾经试过再加一段"批量内写"校验 —— 结论是**不能加在这个位置**，
//   原因见函数体内的说明。要补写语义，必须另找载体与时机并在实机上回归。
static bool ProbeTransferMulti(int dapId)
{
    // 只验证 count>1 的基本语义（读笔数据按序返回）：
    //   连读两次 DP IDCODE，并与单笔结果三方交叉比对。
    //
    // ⚠ 这里**刻意不验证 match 语义**：实测 HSLinkPro/CherryDAP 固件的匹配读
    //   **不往响应里写数据**（`DAP/Source/DAP.c` 的 "Store data" 只在普通读的
    //   else 分支里，:832-836；匹配读分支 :755-788 之后直接出循环），所以
    //   "匹配读 + 取值" 这种用法在该固件上不成立。match 只被用于"等待条件成立"
    //   这一个用途（见 processOneLegacy 的等待值分支），且不依赖它回数据。
    ORBMDK::DAP_XferItem items[2] = { { 0x02, 0 }, { 0x02, 0 } };   // 0x02 = DPACC 读 IDCODE
    uint32_t rd[2] = { 0, 0 };
    int      done  = 0;

    const int st = ORBMDK::DAP_TransferMulti(dapId, items, 2, rd, &done);
    if (st != ORBMDK::DAP_RES_OK || done != 2) {
        LOG_WARN("TransferMulti probe failed (batch of 2 reads: status=%d, done=%d) "
                 "-> 本会话回退逐笔传输", st, done);
        return false;
    }

    uint32_t one = 0;
    if (ORBMDK::DAP_Transfer(dapId, 0x02, &one) != ORBMDK::DAP_RES_OK) {
        LOG_WARN("TransferMulti probe: 单笔交叉比对失败 -> 本会话回退逐笔传输");
        return false;
    }
    if (!((rd[0] == rd[1]) && (rd[0] == one) && one != 0u && one != 0xFFFFFFFFu)) {
        LOG_WARN("TransferMulti probe MISMATCH (batch=0x%08X/0x%08X single=0x%08X) "
                 "-> 本会话回退逐笔传输", rd[0], rd[1], one);
        return false;
    }

    // ⚠ 这里曾加过一段"批量内写"校验（读 DP SELECT -> 同值写回 -> 读回自比）。**已撤回**：
    //   实测在 orbtrace 上，它会让本会话**后续的 AP 存储器访问全部读回 0**
    //   （CPUID / DHCSR / RAM 皆 0；而 AP 寄存器如 CSW 仍正常）。同一固件上：
    //     · 用加固前的 DLL 跑同一 swdprobe = 19/0；
    //     · 用"只加块传输深读、不加本校验"的 DLL = 正常；
    //     · 用加了本校验的 DLL = 14/5。
    //   ⇒ 破坏就出在这一句上（机理未查清）。**不要**再往这里塞写笔：探测发生在
    //     DAP_RegAccessBlock 的 flushBatch 中途，此时 DP/AP 的选择与 posted-read
    //     流水线正处于"半途"状态，额外插写笔容易把它带乱。要补写语义，须换载体
    //     （不要碰 DP SELECT）与时机，并先在 orbtrace 上做同样的实机回归。

    LOG_INFO("TransferMulti probe OK (idcode=0x%08X, count>1 读语义已交叉验证)", one);
    return true;
}

RDDI_FUNC int DAP_RegAccessBlock(const RDDIHandle handle, const int dapId, const int numRegs,
                                 const int *regIdArray, int *dataArray)
{
    LOG_DEBUG("DAP_RegAccessBlock: enter, handle=%d, dapId=%d, numRegs=%d, regIds=%p, data=%p",
              handle, dapId, numRegs, (const void *)regIdArray, (void *)dataArray);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!regIdArray || !dataArray || numRegs <= 0) {
        return RDDI_BADARG;
    }

    // MATCH_MASK(16) / MATCH_RETRY(17) 是虚拟寄存器：只更新本地设置，不产生
    // 总线访问。AGDI 会把它们放在数组最前面（见 SWD_GetARMRegs/SetARMRegs）。
    //
    // matchMaskSet=false 表示本次调用没有显式给出 MATCH_MASK。此时**不能**退化成
    // ~0 做全等比较：AGDI 的等待匹配语义是"期望位置起来了"，例如
    //     regID = DAP_REG_AP_0x0 | DAP_REG_RnW | DAP_REG_WaitForValue
    //     regData = 0x00010000            // 只给期望值，不给掩码
    // 而 DHCSR 读回 0x00030003，全等比较永远不成立，100 次后返回 NO_MATCH，
    // µVision 报 "Erase Failed! / RDDI-DAP Error"。
    // 掩码缺省时按 expected 中为 1 的位匹配（见下方 effectiveMask）。
    bool matchMaskSet = false;
    int  matchMask    = 0;
    int  matchRetry   = 100;

    // ---------------------------------------------------------------------
    // 多笔传输（D2）：把**连续的非等待**寄存器合并进一条 DAP_Transfer 命令。
    //
    // 依据：AGDI 的 SWD_GetARMRegs / SWD_SetARMRegs（单步、运行、命中断点时的
    // 上下文保存/恢复）一次就是十几笔，逐笔发等于十几次 USB 往返；实测每次往返
    // 固定 ≈150 µs（COMPAT_ANALYSIS §9.4 / Todo.md §4 阶段 5）。
    //
    // 语义等价性：DAP_Transfer 按**顺序**执行，同一批量里的 DP_SELECT 之类顺带写也保序
    // （AP posted read 由固件处理），所以与逐笔逐字等价。
    //
    // ⚠ 只用于 SWD：JTAG 下固件的 DAP_Transfer(0x05) 不响应（§18.9），那里
    //    DapTransferFor 走的是本层自己的 JTAG 引擎，无法"合并批量"。
    // ⚠ 能力先探测（ProbeTransferMulti）；失败/中途异常即本会话永久回退逐笔。
    // ---------------------------------------------------------------------
    // 攒批量上限只由**出包长度**决定（乐观估计）。真正的能力判定放在"首次合并批量"时做 ——
    // 否则本次若压根没有可合的笔（例如只来一笔读），也要白付一次探测成本。
    const int multiMax   = ORBMDK::DAP_TransferMultiMax();
    const int batchLimit = ctx->isSWD
                               ? ((multiMax < kMaxMultiBatch) ? multiMax : kMaxMultiBatch)
                               : 0;

    // 批量缓冲按**槽位（slot）**粒度。一个 WaitForValue 读会占两个槽：
    //   [掩码伪写笔(bit5)] + [带 bit4 的匹配读] —— 依据见 ORBMDK_HID.h 的说明。
    std::array<ORBMDK::DAP_XferItem, kMaxMultiBatch> bItem {};
    std::array<uint32_t, kMaxMultiBatch> bR    {};
    std::array<int,      kMaxMultiBatch> bIdx  {};   // 槽 -> 输入下标（-1 = 掩码伪笔）
    std::array<int,      kMaxMultiBatch> bId   {};   // 槽 -> 寄存器 id（LED 跟踪用）
    std::array<int,      kMaxMultiBatch> bRead {};   // 槽是否返回数据
    int nb     = 0;
    int iFirst = -1;    // 本批量覆盖的输入下标区间（中途失配时按区间重做）
    int iLast  = -1;
    // 诊断计数（TESTSPEED 级才落盘）
    int trips   = 0;    // 本次调用实际发生的 USB 往返数
    int nBatch  = 0;    // 其中由"多笔传输"发出的批量数
    int nSingle = 0;    // 逐笔发出的笔数
    int nWait   = 0;    // WaitForValue 笔数

    // 槽位结果落地：读回填、写做 LED 跟踪、掩码伪笔跳过
    auto applySlot = [&](int k) {
        if (bIdx[k] < 0) {
            return;
        }
        if (bRead[k]) {
            dataArray[bIdx[k]] = static_cast<int>(bR[k]);
        } else {
            TrackLedForWrite(ctx, bId[k], bItem[k].data);
        }
    };

    // 单笔老路（含 WaitForValue 的**主机侧轮询**）：单槽批量 / JTAG / 探测失败回退 /
    // 以及"批量中途失配后重做剩余项"都用它 —— 与历史逐笔行为逐字一致。
    auto processOneLegacy = [&](int i) -> int {
        const int regId  = regIdArray[i];
        const int id     = GetRegId(regId);
        const int offset = GetRegOffset(regId);
        if (offset < 0) {
            return RDDI_DAP_BAD_REGISTER_ID;
        }
        const bool isRead = (regId & DAP_REG_RnW) != 0;
        const bool isWait = (regId & DAP_REG_WaitForValue) != 0;

        if (isRead && isWait) {
            // dataArray[i] 是期望值，掩码取自 MATCH_MASK；未给 MATCH_MASK 时
            // 按期望值中为 1 的位匹配（"等这些位置起来"），而不是全等比较。
            const uint8_t request   = static_cast<uint8_t>(offset) | 0x02;
            const int expected      = dataArray[i];
            const int effectiveMask = matchMaskSet ? matchMask : expected;
            const int retries       = (matchRetry > 0) ? matchRetry : 1;
            uint32_t data = 0;
            int status = ORBMDK::DAP_RES_ERROR;
            ++nWait;
            for (int r = 0; r < retries; r++) {
                data = 0;
                ++trips;
                status = DapTransferFor(ctx, dapId, request, &data);
                if (status != ORBMDK::DAP_RES_OK) {
                    break;
                }
                if ((static_cast<int>(data) & effectiveMask) == (expected & effectiveMask)) {
                    break;                          // 条件已满足（快路径：1 次往返）
                }

                // 还没满足。若固件支持 MATCH_VALUE，就让**它**按 match_retry 在内部等，
                // 而不是主机再轮询最多 99 次（每次 1 往返）。
                //   ① 一条命令带 2 槽 = 1 次往返：[掩码伪写笔] + [匹配读]
                //      （匹配读**不回数据**，所以 ② 必须补一次普通读来取值）
                //   ② 普通读 1 次往返 → 拿到"等到之后"的真实值
                // 合计最坏 3 次往返（对比主机轮询最坏 100 次）。
                if (ctx->waitMatchSupported) {
                    ORBMDK::DAP_XferItem mi[2] = {
                        { 0x20u, static_cast<uint32_t>(effectiveMask) },        // 掩码伪写笔
                        { static_cast<uint8_t>(request | 0x10),
                          static_cast<uint32_t>(expected & effectiveMask) }     // 匹配读
                    };
                    ++trips;
                    const int mst = ORBMDK::DAP_TransferMulti(dapId, mi, 2, nullptr, nullptr);
                    if (mst == ORBMDK::DAP_RES_OK) {
                        ++trips;
                        data = 0;
                        status = DapTransferFor(ctx, dapId, request, &data);
                        if (status != ORBMDK::DAP_RES_OK) {
                            break;
                        }
                        if ((static_cast<int>(data) & effectiveMask) ==
                            (expected & effectiveMask)) {
                            break;
                        }
                        // 固件说匹配成功、普通读却仍不匹配 ⇒ 该固件忽略 match 位，别再依赖
                        LOG_WARN("DAP_RegAccessBlock[%d]: 固件 match 结果与读值不一致 -> "
                                 "本会话改用主机侧轮询", i);
                    }
                    ctx->waitMatchSupported = false;
                }
            }
            if (status == ORBMDK::DAP_RES_OK &&
                (static_cast<int>(data) & effectiveMask) != (expected & effectiveMask)) {
                LOG_ERROR("DAP_RegAccessBlock[%d]: no match after %d retries "
                          "(expected 0x%08X, mask 0x%08X%s, got 0x%08X)",
                          i, retries, expected, effectiveMask,
                          matchMaskSet ? "" : " (derived from expected)", data);
                return RDDI_DAP_NO_MATCH;
            }
            if (status != ORBMDK::DAP_RES_OK) {
                return MapDapStatusToRddi(status);
            }
            dataArray[i] = static_cast<int>(data);
            return RDDI_SUCCESS;
        }

        uint32_t data = isRead ? 0u : static_cast<uint32_t>(dataArray[i]);
        const uint8_t request = static_cast<uint8_t>(offset | (isRead ? 0x02 : 0x00));
        ++trips;
        ++nSingle;
        const int status = DapTransferFor(ctx, dapId, request, &data);
        if (status != ORBMDK::DAP_RES_OK) {
            return MapDapStatusToRddi(status);
        }
        if (isRead) {
            dataArray[i] = static_cast<int>(data);
        } else {
            TrackLedForWrite(ctx, id, data);
        }
        return RDDI_SUCCESS;
    };

    // 把已攒的批量发出去；返回 RDDI 码（RDDI_SUCCESS = 全部成功）
    auto flushBatch = [&]() -> int {
        if (nb == 0) {
            return RDDI_SUCCESS;
        }
        const int n = nb;
        const int f = iFirst;
        const int l = iLast;
        nb     = 0;
        iFirst = -1;
        iLast  = -1;

        // ≥2 槽才值得合并批量。首次合并批量前先探测能力（含 match/掩码语义）；
        // 探测失败 → 本批量按逐笔老路重做（顺序与语义完全不变，只是慢）。
        const bool wantMulti = (n >= 2);
        if (wantMulti && !ctx->transferMultiProbed) {
            ctx->transferMultiProbed    = true;
            ctx->transferMultiSupported = ProbeTransferMulti(dapId);
        }

        if (wantMulti && ctx->transferMultiSupported) {
            int done = 0;
            ++trips;                                   // 合并批量：n 槽 = 1 次往返
            ++nBatch;
            const int status = ORBMDK::DAP_TransferMulti(dapId, bItem.data(), n, bR.data(), &done);

            if (status == ORBMDK::DAP_RES_OK && done == n) {
                for (int k = 0; k < n; ++k) {
                    applySlot(k);
                }
                return RDDI_SUCCESS;
            }

            if (status == ORBMDK::DAP_RES_VALUE_MISMATCH) {
                // ★ 等值没等到 —— 这是 match 的**正常语义**，不是故障：
                //   已完成的槽照收，剩余项按逐笔老路重做（老路自己做主机侧轮询）。
                //
                // 但"首槽就没执行"要区分两种情况：首槽若是**普通笔**却没执行，说明固件
                // 根本没按 count>1 处理（不可信，永久回退）；首槽若本身就是匹配读，
                // 首槽失配是正常现象，不当作故障。
                if (done == 0 && n >= 2 && (bItem[0].request & 0x10) == 0) {
                    ctx->transferMultiSupported = false;
                    LOG_WARN("DAP_RegAccessBlock: 多笔传输首槽即未执行 (n=%d) "
                             "-> 本会话回退逐笔传输", n);
                }
                for (int k = 0; k < done; ++k) {
                    applySlot(k);
                }
                int regStart = -1;
                for (int k = done; k < n; ++k) {
                    if (bIdx[k] >= 0) {
                        regStart = bIdx[k];
                        break;
                    }
                }
                if (regStart < 0) {
                    return RDDI_SUCCESS;               // 没有待做的寄存器项
                }
                LOG_DEBUG("DAP_RegAccessBlock: 匹配失配于槽 %d/%d -> 下标 %d..%d 按逐笔重做",
                          done, n, regStart, l);
                for (int i = regStart; i <= l; ++i) {
                    const int rc = processOneLegacy(i);
                    if (rc != RDDI_SUCCESS) {
                        return rc;
                    }
                }
                return RDDI_SUCCESS;
            }

            // 其它提前中断（FAULT/WAIT/NO_ACK/莫名停下）：按失败上抛，与逐笔路径在
            // 首个失败处返回一致；只在"完全没有进展"时才判定固件有问题并永久回退。
            if (done == 0) {
                ctx->transferMultiSupported = false;
            }
            LOG_WARN("DAP_RegAccessBlock: 多笔传输异常 (status=%d, done=%d/%d)",
                     status, done, n);
            return MapDapStatusToRddi(status != ORBMDK::DAP_RES_OK ? status
                                                                  : ORBMDK::DAP_RES_ERROR);
        }

        // 逐笔：单槽批量 / JTAG 会话 / 探测失败 / 已回退 —— 与历史行为完全一致
        if (f >= 0 && l >= f) {
            for (int i = f; i <= l; ++i) {
                const int rc = processOneLegacy(i);
                if (rc != RDDI_SUCCESS) {
                    return rc;
                }
            }
        }
        return RDDI_SUCCESS;
    };

    for (int i = 0; i < numRegs; i++) {
        const int regId = regIdArray[i];
        const int id    = GetRegId(regId);

        if (id == 16) {              // MATCH_MASK：虚拟寄存器，先落地已攒的批量
            const int rc = flushBatch();
            if (rc != RDDI_SUCCESS) {
                return rc;
            }
            matchMask    = dataArray[i];
            matchMaskSet = true;
            continue;
        }
        if (id == 17) {              // MATCH_RETRY
            const int rc = flushBatch();
            if (rc != RDDI_SUCCESS) {
                return rc;
            }
            matchRetry = dataArray[i];
            continue;
        }

        const int offset = GetRegOffset(regId);
        if (offset < 0) {
            // 先把前面已攒的发掉，保持"顺序执行、在坏项处停下"的历史行为
            (void)flushBatch();
            LOG_ERROR("DAP_RegAccessBlock[%d]: unsupported regID=0x%08X", i, regId);
            return RDDI_DAP_BAD_REGISTER_ID;
        }

        const bool isRead = (regId & DAP_REG_RnW) != 0;
        const bool isWait = (regId & DAP_REG_WaitForValue) != 0;

        if (batchLimit <= 0) {
            // 未启用批量处理（JTAG / 已回退）：逐笔，与历史行为一致
            const int rc = processOneLegacy(i);
            if (rc != RDDI_SUCCESS) {
                return rc;
            }
            continue;
        }

        if (isRead && isWait) {
            // 等待值匹配**不编进批量**：实测 HSLinkPro/CherryDAP 固件的匹配读
            // **不往响应里写数据**（`DAP/Source/DAP.c:755-788` 之后直接出循环，
            // 而 "Store data" 只在普通读分支 :832-836），所以"匹配读 + 取值"不成立。
            // 处理：先把已攒的发掉，再走单笔老路 —— 老路内部有"固件 match 加速"
            // （未满足时让固件等 1 往返 + 补一次普通读，把最坏 100 次压到 3 次）。
            const int rc0 = flushBatch();
            if (rc0 != RDDI_SUCCESS) {
                return rc0;
            }
            const int rc1 = processOneLegacy(i);
            if (rc1 != RDDI_SUCCESS) {
                return rc1;
            }
            continue;
        }

        if (nb >= batchLimit) {
            const int rc = flushBatch();
            if (rc != RDDI_SUCCESS) {
                return rc;
            }
        }
        if (iFirst < 0) {
            iFirst = i;
        }
        iLast = i;
        bItem[nb].request = static_cast<uint8_t>(offset | (isRead ? 0x02 : 0x00));
        bItem[nb].data    = isRead ? 0u : static_cast<uint32_t>(dataArray[i]);
        bR[nb]    = 0;
        bIdx[nb]  = i;
        bId[nb]   = id;
        bRead[nb] = isRead ? 1 : 0;
        ++nb;
    }

    {
        const int rc = flushBatch();
        if (rc != RDDI_SUCCESS) {
            return rc;
        }
    }

    // 诊断（TESTSPEED 级，只落日志文件）：一次 RegAccessBlock 到底发了几个往返，以及
    // **被谁切碎**（batch = 合并批量发出的批量数 / single = 逐笔 / wait = 等值匹配笔）。
    // ⚠ 只在 numRegs ≥ 4 时打：2~3 笔的调用是热路径（实测一次会话上万次），逐笔打会把
    //   日志冲垮（首次实测 12k 行里 11944 行都是 `numRegs=2 -> 1`）。
    if (numRegs >= 4 && ORBMDK_LogMeterEnabled()) {
        ORBMDK_LOG_AT_TESTSPEED("RDDI",
                                "RegAccessBlock: numRegs=%d -> %d round trip(s) "
                                "[batch=%d single=%d wait=%d]%s",
                                numRegs, trips, nBatch, nSingle, nWait,
                                ctx->transferMultiSupported ? " (multi-transfer on)" : "");
    }

    return RDDI_SUCCESS;
}


// ---------------------------------------------------------------------------
// DAP_RegWriteRepeat / DAP_RegReadRepeat —— 烧录主力路径
//
// AGDI 的 SWD_WriteBlock / SWD_VerifyBlock / SWD_ReadBlock 走这两个接口
// （见本文第 2.2 节）。旧实现是逐字调用 DAP_Transfer，每个字一次 USB 往返；
// HID 中断端点 Full Speed 轮询间隔 1 ms，于是 1 KB 数据要 256 次往返 ——
// 这是烧录慢的**唯一主因**（见第九节）。
//
// 改为块传输（ID_DAP_TRANSFER_BLOCK）后，单次往返的字数提升到
// BlockWordsLimit()：HID 下 kMaxBlockWords(=14)，V2 Bulk 下按自标定出包长度可达
// 250 字。AGDI 侧已把 CSW 配成地址自增（SWD_WriteBlock / SWD_VerifyBlock），
// 语义与块传输一致。
//
// 块传输**只在 SWD 下启用**（HID 与 V2 Bulk 两条传输层共用
// ORBMDK_HID_DAPCommand 派发）；JTAG 恒走本层逐字引擎 DapTransferFor
// （见 EnsureBlockTransferProbed 的说明）。
//
// 固件若不支持该命令，首次探测失败即永久回退到逐字传输，功能不受影响。
// ---------------------------------------------------------------------------
RDDI_FUNC int DAP_RegWriteRepeat(const RDDIHandle handle, const int dapId, const int numRepeats,
                                 const int regId, const int *dataArray)
{
    LOG_DEBUG("DAP_RegWriteRepeat: enter, handle=%d, dapId=%d, numRepeats=%d, regId=0x%08X, data=%p",
              handle, dapId, numRepeats, regId, (const void *)dataArray);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    const int offset = GetRegOffset(regId);
    if (offset < 0) {
        return RDDI_DAP_BAD_REGISTER_ID;
    }
    if (!dataArray || numRepeats <= 0) {
        return RDDI_BADARG;
    }
    // AP bank 由调用方通过 DAP_WriteReg(DP_SELECT) 管理，此处不改写 DP SELECT
    const uint8_t request = static_cast<uint8_t>(offset);  // Write

    // 首次使用时探测固件是否支持 ID_DAP_TRANSFER_BLOCK；不支持则自动回退
    EnsureBlockTransferProbed(ctx, dapId);
    PublishMeterConfig(ctx);          // 能力落定后（或传输层变化时）补一条 meter config

    const int meterOn = ORBMDK_LogMeterEnabled();
    const int wordsLimit = BlockWordsLimit();
    for (int done = 0; done < numRepeats; ) {
        int chunk = numRepeats - done;
        if (chunk > wordsLimit) {
            chunk = wordsLimit;
        }

        if (ctx->blockTransferSupported) {
            if (meterOn) {
                ORBMDK_LogMeterTripBegin();   // 墙钟从这一笔开始（见 ORBMDK_Log.h）
            }
            const unsigned long long t0 = meterOn ? ORBMDK_LogMeterNowUs() : 0ull;
            const int status = ORBMDK::DAP_TransferBlock(
                dapId, static_cast<uint16_t>(chunk), request,
                reinterpret_cast<const uint32_t*>(dataArray + done), nullptr);
            if (status == ORBMDK::DAP_RES_OK) {
                if (meterOn) {
                    ORBMDK_LogMeterAccount(1, (unsigned)(chunk * 4), 1u,
                                           ORBMDK_LogMeterNowUs() - t0);
                }
                done += chunk;
                continue;
            }
            ctx->blockTransferSupported = false;
            LOG_WARN("DAP_RegWriteRepeat: block transfer failed (status=%d), "
                     "falling back to single transfers", status);
        }

        // ⛔ 此处原有「JTAG 位流批量写」（JtagApWriteBurst）分支，已**整体删除**。
        //    原因见 RDDIContext 顶部注释：WAIT 必须原样重发同一笔（orbtrace 门级
        //    cmsis_dap.py:712-716），位流一次连发 N 笔在语义上不可能正确。
        //    JTAG 的合法批量只能走固件门级的 ID_DAP_TRANSFER（DAP_TransferMulti），
        //    其 `batchLimit` 对 JTAG 目前被关掉（见下方 RegAccessBlock 的 `: 0`）。
        {
            if (meterOn) {
                ORBMDK_LogMeterTripBegin();
            }
            const unsigned long long t0 = meterOn ? ORBMDK_LogMeterNowUs() : 0ull;
            for (int i = 0; i < chunk; i++) {
                uint32_t data = static_cast<uint32_t>(dataArray[done + i]);
                const int status = DapTransferFor(ctx, dapId, request, &data);
                if (status != ORBMDK::DAP_RES_OK) {
                    LOG_ERROR("DAP_RegWriteRepeat: single write failed at %d/%d, status=%d",
                              done + i, numRepeats, status);
                    return RDDI_DAP_ERROR;      // 失败的那一次不记账
                }
            }
            if (meterOn) {
                ORBMDK_LogMeterAccount(1, (unsigned)(chunk * 4), (unsigned)chunk,
                                       ORBMDK_LogMeterNowUs() - t0);
            }
        }
        done += chunk;

        // JTAG 的写是流水线的：每个 chunk 收尾补一次访问，把挂起的那笔写真正落到
        // 目标上（SWD 下 JtagDapFlush 立即返回，无开销）
        JtagDapFlush(ctx);
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_RegReadRepeat(const RDDIHandle handle, const int dapId, const int numRepeats,
                                const int regId, int *dataArray)
{
    LOG_DEBUG("DAP_RegReadRepeat: enter, handle=%d, dapId=%d, numRepeats=%d, regId=0x%08X, data=%p",
              handle, dapId, numRepeats, regId, (void *)dataArray);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    const int offset = GetRegOffset(regId);
    if (offset < 0) {
        return RDDI_DAP_BAD_REGISTER_ID;
    }
    if (!dataArray || numRepeats <= 0) {
        return RDDI_BADARG;
    }
    // AP bank 由调用方通过 DAP_WriteReg(DP_SELECT) 管理，此处不改写 DP SELECT
    const uint8_t request = static_cast<uint8_t>(offset) | 0x02;  // Read

    // 首次使用时探测固件是否支持 ID_DAP_TRANSFER_BLOCK；不支持则自动回退
    EnsureBlockTransferProbed(ctx, dapId);
    PublishMeterConfig(ctx);          // 能力落定后（或传输层变化时）补一条 meter config

    const int meterOn = ORBMDK_LogMeterEnabled();
    const int wordsLimit = BlockWordsLimit();
    for (int done = 0; done < numRepeats; ) {
        int chunk = numRepeats - done;
        if (chunk > wordsLimit) {
            chunk = wordsLimit;
        }

        if (ctx->blockTransferSupported) {
            // 先清零本块：块传输若只回部分数据，剩余元素不会残留上一次的值
            for (int i = 0; i < chunk; i++) {
                dataArray[done + i] = 0;
            }
            // 计时从清零之后开始：只算 DAP 传输段，主机侧清理不计入
            if (meterOn) {
                ORBMDK_LogMeterTripBegin();
            }
            const unsigned long long t0 = meterOn ? ORBMDK_LogMeterNowUs() : 0ull;
            const int status = ORBMDK::DAP_TransferBlock(
                dapId, static_cast<uint16_t>(chunk), request,
                nullptr, reinterpret_cast<uint32_t*>(dataArray + done));
            if (status == ORBMDK::DAP_RES_OK) {
                if (meterOn) {
                    ORBMDK_LogMeterAccount(0, (unsigned)(chunk * 4), 1u,
                                           ORBMDK_LogMeterNowUs() - t0);
                }
                done += chunk;
                continue;
            }
            ctx->blockTransferSupported = false;
            LOG_WARN("DAP_RegReadRepeat: block transfer failed (status=%d), "
                     "falling back to single transfers", status);
        }

        {
            if (meterOn) {
                ORBMDK_LogMeterTripBegin();
            }
            const unsigned long long t0 = meterOn ? ORBMDK_LogMeterNowUs() : 0ull;
            for (int i = 0; i < chunk; i++) {
                uint32_t data = 0;
                const int status = DapTransferFor(ctx, dapId, request, &data);
                if (status != ORBMDK::DAP_RES_OK) {
                    LOG_WARN("DAP_RegReadRepeat: single read failed at %d/%d, status=%d",
                              done + i, numRepeats, status);
                    return RDDI_DAP_ERROR;      // 失败的那一次不记账
                }
                dataArray[done + i] = static_cast<int>(data);
            }
            if (meterOn) {
                ORBMDK_LogMeterAccount(0, (unsigned)(chunk * 4), (unsigned)chunk,
                                       ORBMDK_LogMeterNowUs() - t0);
            }
        }
        done += chunk;
    }

    return RDDI_SUCCESS;
}

// ============================================================================
// CMSIS-DAP Specific Functions
// ============================================================================

// ---------------------------------------------------------------------------
// 对外暴露的"接口列表" = 可用的**传输层**，与 AGDI 对话框一一对应
//
//   接口 0 = CMSIS-DAP v2 (USB Bulk)
//   接口 1 = CMSIS-DAP v1 (HID)
//
// AGDI 的处理（反汇编 0x1002203C，见 COMPAT_ANALYSIS §14）：
//   CMSIS_DAP_Detect(h, &n)                       ; n 就是个列表项数
//   for (ifNo = 0; ifNo < min(n, 16); ++ifNo) {   ; 最多 16 项
//       Identify(h, ifNo, 2, buf, 0x104)          ; 产品名 -> 列表文字
//       Identify(h, ifNo, 3, buf, 0x104)          ; 序列号
//       entry[count].ifNo = ifNo;                 ; 记住序号
//   }
//   用户选中某项后：ConfigureInterface(h, [选中项的 ifNo], cfg)
//
// 所以"两种模式都显示、可切换"= Detect 报 2 + Identify 按 ifNo 给不同名字
//   + ConfigureInterface 按 ifNo 切传输层。
// ---------------------------------------------------------------------------
static constexpr int kTransportInterfaceCount = 2;

// ---------------------------------------------------------------------------
// 运行期可改的接口数量（默认 2，行为不变）
//
//   文件 %TEMP%\ORBMDK_IFACES   内容 1 或 2
//   环境变量 ORBMDK_IFACES
//
// 存在的意义：Keil 的 PDSC 层在"多调试端口"场景下要按端口身份（GUID / 端口 ID）
// 做匹配。把**同一个物理调试器**的两种传输报成两条接口，会让它走多端口分支
// （现场症状：PDSC: Unknown Debug Port ID / Cannot switch to Debug Port）。
// 设为 1 可一次性判定"是否多接口暴露导致"；默认仍为 2。
// ---------------------------------------------------------------------------
static int TransportInterfaceCount(void)
{
    static int cached = 0;
    if (cached > 0) {
        return cached;
    }
    cached = kTransportInterfaceCount;

    char raw[32] = {0};
    char path[MAX_PATH] = {0};
    const DWORD n = GetTempPathA((DWORD)sizeof(path), path);
    if (n > 0 && n < sizeof(path)) {
        strncat_s(path, sizeof(path), "ORBMDK_IFACES", _TRUNCATE);
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") == 0) {
            if (fgets(raw, sizeof(raw), f) == nullptr) {
                raw[0] = '\0';
            }
            fclose(f);
        }
    }
    if (raw[0] == '\0') {
        const DWORD got = GetEnvironmentVariableA("ORBMDK_IFACES", raw, (DWORD)sizeof(raw));
        if (got == 0 || got >= sizeof(raw)) {
            raw[0] = '\0';
        }
    }
    if (raw[0] != '\0') {
        const int v = atoi(raw);
        if (v >= 1 && v <= kTransportInterfaceCount) {
            cached = v;
        }
    }
    return cached;
}

RDDI_FUNC int CMSIS_DAP_Detect(const RDDIHandle handle, int *noOfIFs)
{
    LOG_DEBUG("CMSIS_DAP_Detect: enter, handle=%d, noOfIFs=%p", handle, (void *)noOfIFs);
    // 即使失败也写入输出参数：调用方（AGDI 的 PDSCDebug_InitDebugger）
    // 只检查 *noOfIFs == 0 而不看返回值，若不写会读到调用方的初始值 0，
    // 从而把 "句柄无效" 误报成 "No Debug Unit Found"。
    if (noOfIFs) {
        *noOfIFs = 0;
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // ⚠️ 绝不能返回 0：AGDI 看到 0 会直接以 EU02（"No Debug Unit Found"）中止
    // 整个初始化（见 §4.6）。没有设备时也报 1，让用户能进对话框看到错误。
    *noOfIFs = TransportInterfaceCount();
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_Identify(const RDDIHandle handle, int ifNo, int idNo,
                                   char *str, const int len)
{
    LOG_DEBUG("CMSIS_DAP_Identify: enter, handle=%d, ifNo=%d, idNo=%d, len=%d",
              handle, ifNo, idNo, len);
    // 先清空输出缓冲：任何失败路径下调用方都不应读到未初始化内存
    // （AGDI 会对该字符串直接做 strcmp / strncpy）。
    if (str && len > 0) {
        str[0] = '\0';
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!str || len <= 0) {
        return RDDI_BADARG;
    }

    // 优先使用 RDDI_Open 时取到的设备标识；取不到再向固件查询；
    // 最后才用硬编码兜底。
    //
    // ifNo 决定"这是哪一个传输接口"：0 = CMSIS-DAP v2 (Bulk)，1 = CMSIS-DAP v1 (HID)。
    // AGDI 的对话框就是靠这里的**产品名**把两个接口显示成两条可选适配器的，
    // 所以 idNo=2 时必须按 ifNo 给出不同名字（见 CMSIS_DAP_Detect 的说明）。
    if (ifNo < 0 || ifNo >= TransportInterfaceCount()) {
        LOG_WARN("CMSIS_DAP_Identify: ifNo=%d out of range [0,%d), empty result",
                 ifNo, TransportInterfaceCount());
        return RDDI_SUCCESS;   // 保持空串（已在上面清空）
    }

    switch (idNo) {
        case 1:  // Vendor (RDDI_CMSIS_DAP_ID_VENDOR)
            ORBMDK::DAP_GetInfo(DAP_INFO_VENDOR, str, len);
            if (str[0] == '\0') {
                strncpy_s(str, len, "ORBTrace", len - 1);
            }
            break;

        case 2:  // Product (RDDI_CMSIS_DAP_ID_PRODUCT) —— 对话框里的列表文字
            // 名字的唯一来源是 GetInterfaceName（里面已经按 ifNo 如实判定，包括
            // "总线上有没有 v1 候选"这一问）。下面的兜底按构造不可达 —— str/len
            // 已在本函数入口校验过，GetInterfaceName 对这些参数不会失败 ——
            // 留着只为防御，别在这里另起一套命名逻辑。
            if (ORBMDK_USB_Bulk_GetInterfaceName(ifNo, str, len) != 0 ||
                str[0] == '\0') {
                strncpy_s(str, len, (ifNo == 1) ? "CMSIS-DAP v1" : "CMSIS-DAP v2", len - 1);
            }
            break;

        case 3:  // Serial number (RDDI_CMSIS_DAP_ID_SER_NUM)
            // ifNo=1（v1/HID）**且总线上并没有 v1 接口**时，绝不能把 V2 的序列号
            // 套到这条幽灵条目上 —— 那样它在适配器列表里就是一台"带正确序列号的
            // 真 DAP"，用户没有理由不选它（选中必然在 ConfigureInterface 自检失败）。
            // 真正存在的 v1 接口（或 dual-mode 设备的 v1 侧）仍共用同一份序列号：
            // 两个接口属于同一个物理设备。
            if (ifNo == 1 && ORBMDK::ORBMDK_HID_HasV1Candidate() <= 0) {
                strncpy_s(str, len, "Unknown", len - 1);
                break;
            }
            if (!ctx->serialNumber.empty()) {
                strncpy_s(str, len, ctx->serialNumber.c_str(), len - 1);
            } else {
                ORBMDK::DAP_GetInfo(DAP_INFO_SERIAL, str, len);
                if (str[0] == '\0') {
                    strncpy_s(str, len, "Unknown", len - 1);
                }
            }
            break;

        case 4:  // Firmware version (RDDI_CMSIS_DAP_ID_FW_VER) —— 实回 CMSIS-DAP 协议版本
            // ifNo=1 且没有 v1 接口时（同 case 3）：不回当前设备的版本串，
            // 避免给幽灵 v1 背书。回兜底串（主版本 < 2，不会打开 streaming 门控），
            // 与"这台接口不可用"的语义一致。
            if (ifNo == 1 && ORBMDK::ORBMDK_HID_HasV1Candidate() <= 0) {
                strncpy_s(str, len, kFallbackProtocolVersion, len - 1);
                break;
            }
            // 一律回 RDDI_Open 缓存的那一份（设备 `DAP_Info(0x04)` 的协议版本串 +
            // 主版本归一化）。**不要**改成每次现问设备：AGDI 拿返回值做能力判定，
            // 两次调用给出不同的串会出事（COMPAT_ANALYSIS 条目 50）。
            // 这里的 else 只是防御性兜底。
            if (!ctx->protocolVersion.empty()) {
                strncpy_s(str, len, ctx->protocolVersion.c_str(), len - 1);
            } else {
                strncpy_s(str, len, kFallbackProtocolVersion, len - 1);
            }
            break;

        default:
            // 5 = Device Vendor / 6 = Device Name：由上层 AGDI 从 PDSC 提供，
            // 本层无法回答，保持空串（已在上方清空）。
            break;
    }

    // 入口/出口都留痕：AGDI 在"多接口"路径里就是靠这里取适配器与设备身份，
    // 报 "Unknown Debug Port ID" 时第一个要看的就是 idNo 与返回的串。
    LOG_DEBUG("CMSIS_DAP_Identify: ifNo=%d, idNo=%d -> '%s'", ifNo, idNo, str);
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_ConfigureInterface(const RDDIHandle handle, int ifNo, char *str)
{
    LOG_DEBUG("CMSIS_DAP_ConfigureInterface: enter, handle=%d, ifNo=%d, cfg=%p",
              handle, ifNo, (void *)str);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_ConfigureInterface: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    // ★ 这里就是"用户在对话框里选了哪个适配器"的落点（AGDI 把选中项的 ifNo
    //   原样传进来，反汇编 0x10022AA8）。据此把传输层切到对应模式：
    //     ifNo 0 -> CMSIS-DAP v2 (Bulk)    ifNo 1 -> CMSIS-DAP v1 (HID)
    // 必须在 DAP_Configure / CMSIS_DAP_Connect **之前**完成切换。
    if (ifNo >= 0 && ifNo < TransportInterfaceCount()) {
        const int sel = ORBMDK_USB_Bulk_SelectInterface(ifNo);
        if (sel != 0) {
            // 选中的通道打不开：**如实报错并中止本次配置**，不偷偷换到另一条。
            // 静默降级会把"V2 坏了"伪装成"一切正常"（历史上正是如此掩盖了句柄
            // 泄漏导致的 err=5：整场会话看着能用，其实一直跑在 V1 上）。
            LOG_ERROR("CMSIS_DAP_ConfigureInterface: interface %d (%s) could not be opened (%d) "
                      "- NOT falling back to the other transport",
                      ifNo, (ifNo == 1) ? "CMSIS-DAP v1/HID" : "CMSIS-DAP v2/Bulk", sel);
            return RDDI_FAILED;
        }
    } else {
        LOG_WARN("CMSIS_DAP_ConfigureInterface: ifNo=%d out of range, transport unchanged",
                 ifNo);
    }

    // Parse configuration string like:
    // "Master=Y;Port=SW;SWJ=Y;Clock=10000000;..."

    if (!str) {
        return RDDI_SUCCESS;
    }

    // Parse key=value pairs
    const char *p = str;
    while (*p) {
        // Skip to '='
        while (*p && *p != '=') p++;
        if (!*p) break;

        // key 的起点回退到本段起始处。注意 p == str 时不能写成 p - 1，
        // 否则指针下溢，keyStr 会读到缓冲区之前的内存。
        const char *key = (p > str) ? p - 1 : str;
        while (key > str && *(key - 1) != ';') key--;

        p++;  // Skip '='

        const char *value = p;
        while (*p && *p != ';') p++;

        // key 指向 key 首字符，value 指向 '=' 后的首字符；
        // 因此 key 的实际结束位置是 value - 1（即 '=' 之前）。
        // 空 key（形如 "=x;"）时 keyEnd 取 key，长度 0，不越界。
        const char *keyEnd = (value > key) ? value - 1 : key;
        std::string keyStr(key, static_cast<size_t>(keyEnd - key));
        std::string valueStr(value, static_cast<size_t>(p - value));

        if (keyStr == "Port") {
            // 这里决定后面走 SWD 还是 JTAG 建链（§18.9）：CMSIS_DAP_Connect /
            // DetectTargetDapIdList / LinkRecover 都会读 ctx->isSWD 分岔。
            const bool newIsSwd = (valueStr == "SW");

            // 模式切换让上一次的结论与 JTAG 链状态全部失效：
            //   - 块传输只在 SWD 下启用，切模式后必须让探测结论失效
            //     （EnsureBlockTransferProbed 会按新模式重新落定：SWD 探测、
            //      JTAG 直接置不支持）；
            //   - IR 缓存 / 写流水线是 JTAG 专属状态，换链后必须作废。
            if (newIsSwd != ctx->isSWD) {
                ctx->blockTransferProbed    = false;
                ctx->blockTransferSupported = true;   // 乐观开启，按新模式重新落定
                // 多笔传输同样只在 SWD 下可用：换模式后探测结论与等待加速一并重新判定
                ctx->transferMultiProbed    = false;
                ctx->transferMultiSupported = true;
                ctx->waitMatchSupported     = true;
                ctx->jtagCurIr        = 0;
                ctx->jtagWritePending = false;
                LOG_INFO("CMSIS_DAP_ConfigureInterface: 调试模式切换 -> 块传输/多笔传输能力将重新判定");
            }

            ctx->isSWD = newIsSwd;
            LOG_INFO("CMSIS_DAP_ConfigureInterface: Port='%s' -> %s 模式",
                     valueStr.c_str(), ctx->isSWD ? "SWD" : "JTAG");
        } else if (keyStr == "Clock") {
            try {
                const unsigned long hz = std::stoul(valueStr);
                // 只接受合理范围：AGDI 会发 Clock=1000000 / Clock=100000，
                // 而 "Clock=0" 或溢出值会把 SWJ 时钟设坏。
                if (hz > 0UL && hz <= 200000000UL) {
                    ctx->debugClock = static_cast<uint32_t>(hz);
                }
            } catch (...) {
                // Ignore parse errors
            }
        }

        if (*p) p++;  // Skip ';'
    }

    // Apply configuration
    ORBMDK::DAP_SetSWJClock(ctx->debugClock);

    return RDDI_SUCCESS;
}

// 标准函数：配置 DAP
RDDI_FUNC int CMSIS_DAP_ConfigureDAP(const RDDIHandle handle, const char *str)
{
    LOG_DEBUG("CMSIS_DAP_ConfigureDAP: enter, handle=%d, str=%p", handle, (const void *)str);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 解析配置字符串，如 "SWJSwitch=0xE79E"
    if (!str) {
        return RDDI_SUCCESS;
    }

    // 简单解析 SWJSwitch
    if (strncmp(str, "SWJSwitch=", 10) == 0) {
        // JTAG-to-SWD 切换序列
        // 这是 JTAG-to-SWD 切换时需要的默认序列
    }

    return RDDI_SUCCESS;
}

// 标准函数：获取 DAP 能力
RDDI_FUNC int CMSIS_DAP_Capabilities(const RDDIHandle handle, int ifNo, int *cap_info)
{
    LOG_DEBUG("CMSIS_DAP_Capabilities: enter, handle=%d, ifNo=%d", handle, ifNo);
    if (cap_info) {
        *cap_info = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!cap_info) {
        return RDDI_BADARG;
    }

    // 返回支持的协议能力
    // Bit 0: SWD, Bit 1: JTAG, Bit 4: Atomic Commands
    //
    // ⚠️ 这里**只宣称已验证的能力**：JTAG 代码路径已实现（§18.9），但目标侧
    // 还没验证通过（扫链为空，见 §18.9 的实测记录），所以暂时**不加** JTAG 位 ——
    // 宣称未验证的能力等于骗宿主。（Keil 的 Port 下拉框并不按能力位限制，
    // 因此需要试 JTAG 时照样可选：本层会按 ConfigureInterface 的 Port= 走 JTAG 路径。）
    // JTAG 在真机上验证通过后，把 `| INFO_CAPS_JTAG` 加回来。
    //
    // SWO 位（Bit2/Bit3）是 AGDI trace 配置块的**门控位**（反汇编见 COMPAT_ANALYSIS.md §17.9(8)）：
    //   0x10022A2F  test caps, 0x04   ; INFO_CAPS_SWO_UART
    //   0x10022A53  test caps, 0x08   ; INFO_CAPS_SWO_MANCHESTER
    // 两位都置 0 时，AGDI 找不到可用的 Trace Port，会把传输兜底写成 None
    // （配置串 `Trace=Off;TraceBaudrate=0;TraceTransport=None;`），界面上的
    // `Trace Enable` 勾选因此不产生任何效果。
    //
    // Read 传输只需上面两位之一，**不**要求 Identify(idNo=4) 主版本 >= 2，
    // 也**不**要求 INFO_CAPS_SWO_STREAMING_TRACE(0x40)。
    //
    // Streaming 位（Bit6 = 0x40）**已置**（2026-10-01 放开，见 Todo.md.bak §18.10-C）。
    // 它与 Identify(idNo=4) 的主版本号是同一道门控的两半，两道必须同侧：
    //   0x1003CB97  cmp eax, 2 / jb   —— 主版本 >= 2 才走 streaming sink 注册
    //   0x10022A6B  test caps, 0x40   —— caps 无 0x40 时 Trace 传输退回 Read
    // 版本那一半已由 ORBMDK_NormalizeProtocolVersion 统一提升到 >= 2，故这里必须一起打开；
    // 只开一半（版本 1 + 有 0x40）等于向 AGDI 宣称了流式能力，却永远等不到它来注册 sink。
    // 回退时两处一起改回，具体步骤见 include/ORBMDK.h 里那段 ⚠️ 回退说明。
    //
    // ★ 但"置位"的前提是**探针自己承认**：本层是"探针 → 宿主"的中间层，不能替固件
    //   宣称它没有的能力（否则 AGDI 按能力位下发 SWO 配置，探针答不上来，界面显示
    //   已启用却一条数据都没有）。故按 RDDI_Open 缓存的探针自报能力位 probeCaps 收口：
    //     - probeCaps >= 0：逐位对齐探针自报（探针没报的 SWO 位一律不置）；
    //     - probeCaps < 0（老固件不认 DAP_Info 0xF0）：沿用乐观口径，不按探针收口。
    //   实测 CherryDAP 固件对 0xF0 回 `00 02 33 01` -> caps=0x0133：
    //   SWD|JTAG|SWO_UART?（0x04 未置！0x33 = 0x01|0x02|0x10|0x20）
    int caps = INFO_CAPS_SWD | INFO_CAPS_ATOMIC_CMDS;
    if (ctx->probeCaps >= 0) {
        if (ctx->probeCaps & INFO_CAPS_SWO_UART)            caps |= INFO_CAPS_SWO_UART;
        if (ctx->probeCaps & INFO_CAPS_SWO_MANCHESTER)      caps |= INFO_CAPS_SWO_MANCHESTER;
        if (ctx->probeCaps & INFO_CAPS_SWO_STREAMING_TRACE) caps |= INFO_CAPS_SWO_STREAMING_TRACE;
        LOG_INFO("CMSIS_DAP_Capabilities: 按探针自报 0x%04X 收口 -> 0x%04X"
                 "(SWO_UART=%d SWO_MANCHESTER=%d SWO_STREAM=%d)",
                 static_cast<unsigned>(ctx->probeCaps), static_cast<unsigned>(caps),
                 (caps & INFO_CAPS_SWO_UART) ? 1 : 0,
                 (caps & INFO_CAPS_SWO_MANCHESTER) ? 1 : 0,
                 (caps & INFO_CAPS_SWO_STREAMING_TRACE) ? 1 : 0);
    } else {
        caps |= INFO_CAPS_SWO_UART | INFO_CAPS_SWO_MANCHESTER | INFO_CAPS_SWO_STREAMING_TRACE;
        LOG_INFO("CMSIS_DAP_Capabilities: 无探针能力位（probeCaps<0）-> 乐观口径 0x%04X",
                 static_cast<unsigned>(caps));
    }

    *cap_info = caps;

    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// 探测目标 DAP 的 IDCODE，填充 ctx->dapIdList，返回探测到的 DAP 个数。
//
// 关键：AGDI 把 CMSIS_DAP_GetDeviceIDList / CMSIS_DAP_DetectDAPIDList 写回的数组
// 当作 **IDCODE** 使用（填入 JTAG_devs.ic[i].id），而不是 DAP 索引。
// 官方 CMSIS_DAP.dll 反汇编（RVA 0x14FD0）确认它从内部设备表逐个拷贝 4 字节设备 ID：
//     mov  edi, dword ptr [edx+eax]
//     mov  dword ptr [eax], edi
// 旧实现写入 kSingleDapId + i（恒为 0），µVision 于是显示
// "IDCODE 0x00000000 / DeviceName Unknown device"。
// ---------------------------------------------------------------------------
static int DetectTargetDapIdList(RDDIContext *ctx, uint32_t *outIdcode, int *outMode)
{
    if (outIdcode) {
        *outIdcode = 0;
    }
    if (outMode) {
        *outMode = 0;
    }

    // Connect to target (0=默认/自动, 1=SWD, 2=JTAG) —— 按 Port= 的选择分岔（§18.9）。
    // JTAG 下 JtagInitSequence 已下发 IR 长度、复位 TAP 并扫到 IDCODE。
    int mode;
    uint32_t jtagIdcode = 0;
    if (ctx->isSWD) {
        mode = ORBMDK::DAP_ConnectTarget();
    } else {
        int jmode = 0;
        if (!JtagInitSequence(ctx, &jmode, &jtagIdcode)) {
            LOG_ERROR("DetectTargetDapIdList: JTAG 建链失败");
            ctx->dapIdList.clear();
            return 0;
        }
        mode = DAP_CONNECT_JTAG;
    }
    if (mode < 0 || mode > 2) {
        ctx->dapIdList.clear();
        return 0;  // 不返回错误，让调用方决定如何处理
    }
    if (mode == 0) {
        mode = 1;  // 固件返回"默认"时按 SWD 处理（与 CMSIS_DAP_Connect 一致）
    }
    if (outMode) {
        *outMode = mode;
    }

    ORBMDK::DAP_SetSWJClock(ctx->debugClock);

    // 与 CMSIS_DAP_Connect 相同的建链步骤：DAP_Connect(port=SWD) 内部的 line reset
    // 只有 50 周期，部分固件不足以让目标可靠进入 SWD 模式，必须在主机侧补一次标准
    // JTAG-to-SWD 切换序列（≥50 个 1 + 16-bit 0xE79E + ≥50 个 1 + idle）。
    //
    // 历史问题：本路径缺少这一步时 DP 未进入 SWD 模式，IDCODE 读回 0，
    // µVision 显示 "IDCODE 0x00000000 / DeviceName Unknown Device"。
    if (mode == 1) {
        uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // 56 位 1
        uint8_t jtagToSwd[] = { 0x9E, 0xE7 };                                // 16 位 0xE79E (LSB first)
        uint8_t idle[] = { 0x00 };
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(16, jtagToSwd);
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(8, idle);
        ORBMDK::DAP_ConfigureSWD(0);  // Default SWD config
    }
    ORBMDK::DAP_ConfigureTransfer(0, kDefaultWaitRetry, kDefaultMatchRetry);

    // Read IDCODE: DP 寄存器 0x00，读操作 (APnDP=0, RnW=1, A[3:2]=0)
    // 请求字节 = 0x00 | 0x02 = 0x02
    uint32_t idcode = 0;
    int status = DapTransferFor(ctx, 0, 0x02, &idcode);  // Read IDCODE request

    // DPIDR 不可能为 0 或 0xFFFFFFFF。读回这些值说明目标还没进入 SWD 模式，
    // 补一次线复位后重试一次。
    if (status != ORBMDK::DAP_RES_OK || idcode == 0 || idcode == 0xFFFFFFFFu) {
        if (mode == 1) {
            uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            uint8_t idle[] = { 0x00 };
            ORBMDK::DAP_SWJ_Sequence(56, lineReset);
            ORBMDK::DAP_SWJ_Sequence(8, idle);
        }
        status = DapTransferFor(ctx, 0, 0x02, &idcode);
    }

    // ------------------------------------------------------------------
    // 填"器件表" —— AGDI 的 JTAG_devs 就是按这张表画的（行数取
    // CMSIS_DAP_JTAG_GetIDCODEs 的 *count，每行 IDCODE 取 GetDeviceIDList /
    // DetectDAPIDList，两者都读 ctx->dapIdList；见本函数上方说明）：
    //   * JTAG 会话：链上**每个 TAP** 都是一行（本链 = cpu IR=4 + bs IR=5），顺序与
    //     jtagIrLens[] 对齐（[0] = 离 TDO 最近的器件）。只回 DP 一个会让 Keil 的
    //     JTAG 列表少一行（2026-10-03 实测 GetIDCODEs=1 / GetIRLengths=2 的旧缺陷，已修，
    //     修后 jtagprobe 实测两者一致：count=2，id 含 0x4BA00477 + 0x06413041）。
    //   * SWD 会话：**保持原样** —— 单 DP 目标就是 1 项，这段行为不要动。
    // ------------------------------------------------------------------
    ctx->dapIdList.clear();
    if (!ctx->isSWD && ctx->jtagChainCount > 0) {
        for (int i = 0; i < ctx->jtagChainCount && i < 8; ++i) {
            if (ctx->jtagIds[i] != 0) {
                ctx->dapIdList.push_back(ctx->jtagIds[i]);
            }
        }
        // 扫链 IDCODE 缺席（例如本次会话还没扫过链）时退回 DP 那一个，别把表清空
        if (ctx->dapIdList.empty() && jtagIdcode != 0) {
            ctx->dapIdList.push_back(jtagIdcode);
        }
        if (outIdcode && !ctx->dapIdList.empty()) {
            *outIdcode = ctx->dapIdList[0];
        }
        LOG_INFO("DetectTargetDapIdList: JTAG 链上 %d 个 TAP -> 器件表 %llu 项"
                 " (id[0]=0x%08X id[1]=0x%08X)",
                 ctx->jtagChainCount, (unsigned long long)ctx->dapIdList.size(),
                 ctx->jtagIds[0], ctx->jtagIds[1]);
    } else if (status == ORBMDK::DAP_RES_OK && idcode != 0 && idcode != 0xFFFFFFFFu) {
        ctx->dapIdList.push_back(idcode);
        if (outIdcode) {
            *outIdcode = idcode;
        }
    } else if (!ctx->isSWD && jtagIdcode != 0) {
        // JTAG：链上 ID 不可用时，用扫链拿到的 DP IDCODE 兜底
        ctx->dapIdList.push_back(jtagIdcode);
        if (outIdcode) {
            *outIdcode = jtagIdcode;
        }
    }

    // Connect LED：能走到这里说明 DAP_ConnectTarget 已成功，点亮它。
    // （官方 AGDI 不调用 DAP_HostStatus，必须由本层驱动，见 SetHostLed 说明）
    SetHostLed(ctx, kHostLedConnect, true);

    // ------------------------------------------------------------------
    // 块传输能力探测：放在这里是因为**只有这里能确定目标已连接**。
    //
    // 以前探测是"首次用到 DAP_RegWriteRepeat 时才做"，结果它在很多场合
    // 根本不会被执行（例如调用方传了非法 regID 就直接返回了），
    // 于是"固件不支持 ID_DAP_TRANSFER_BLOCK"这个结论**从未被真正验证过**——
    // 它其实只是某次探测失败留下的产物。现在改成每次连上目标都探测一次并留证。
    // ------------------------------------------------------------------
    if (status == ORBMDK::DAP_RES_OK) {
        EnsureBlockTransferProbed(ctx, 0);
        // 目标刚连上：多笔传输能力一并重新判定 —— 上次可能因"目标还没上电"而探测失败，
        // 一旦失败就会在本会话内永久回退逐笔，这里给它一次重新来过的机会（与块传输同法）。
        ctx->transferMultiProbed = false;
        ctx->waitMatchSupported  = true;    // 目标重连后重新判定等待加速
    }
    return static_cast<int>(ctx->dapIdList.size());
}

RDDI_FUNC int CMSIS_DAP_DetectNumberOfDAPs(const RDDIHandle handle, int *numDAPs)
{
    LOG_DEBUG("CMSIS_DAP_DetectNumberOfDAPs: enter, handle=%d, numDAPs=%p",
              handle, (void *)numDAPs);
    if (numDAPs) {
        *numDAPs = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_DetectNumberOfDAPs: invalid handle=%d -> RDDI_INVHANDLE", handle);
        return RDDI_INVHANDLE;
    }

    if (!numDAPs) {
        LOG_WARN("CMSIS_DAP_DetectNumberOfDAPs: null out param -> RDDI_BADARG");
        return RDDI_BADARG;
    }

    *numDAPs = DetectTargetDapIdList(ctx, nullptr, nullptr);

    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：CMSIS_DAP_DetectDAPIDList
//
// 第 3 个参数是 **元素个数**，不是字节数！
//   AGDI（PDSCDebug_DebugGetDeviceList）：
//       rddi::CMSIS_DAP_DetectDAPIDList(k_rddi_handle, idcode_list, noOfDAPs);
//   elaphureLinkRDDI 参考实现同样按个数处理：
//       auto sz = (std::min)(sizeOfArray, idcode_list.size());
//
// 旧版本按字节数处理并带 `sizeOfArray < sizeof(int)` 的前置检查：
// 调用方传 noOfDAPs=1 时 1 < 4 直接返回 RDDI_BADARG，一个元素都没写，
// 调用方读到未初始化的 idcode_list[0] -> µVision 显示
// "IDCODE 0x00000000 / DeviceName Unknown Device"。
// ---------------------------------------------------------------------------
RDDI_FUNC int CMSIS_DAP_DetectDAPIDList(const RDDIHandle handle, int *DAP_ID_Array,
                                          size_t sizeOfArray)
{
    LOG_DEBUG("CMSIS_DAP_DetectDAPIDList: enter, handle=%d, array=%p, sizeOfArray=%llu",
              handle, (void *)DAP_ID_Array, (unsigned long long)sizeOfArray);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_DetectDAPIDList: invalid handle=%d -> RDDI_INVHANDLE", handle);
        return RDDI_INVHANDLE;
    }

    if (!DAP_ID_Array || sizeOfArray == 0) {
        LOG_WARN("CMSIS_DAP_DetectDAPIDList: BADARG (array=%p, requested=%llu)",
                 (void *)DAP_ID_Array, (unsigned long long)sizeOfArray);
        return RDDI_BADARG;
    }

    // 目标尚未探测过时（dapIdList 为空）主动探测一次，避免恒返回 0。
    if (ctx->dapIdList.empty()) {
        DetectTargetDapIdList(ctx, nullptr, nullptr);
    }

    // sizeOfArray = 可写入的元素个数（上限受 dapIdList 长度约束，故两种
    // 解释下都不会越界：按个数解释写 <=1 个，按字节数解释更宽松）。
    const size_t count = std::min(sizeOfArray, ctx->dapIdList.size());

    if (count == 0) {
        // 目标未探测到（dapIdList 为空）时也要写入，避免调用方读到未初始化值。
        // 注意 AGDI 的 SWD_ReadID / JTAG_DetectDevices 会把该值当作 IDCODE 使用。
        DAP_ID_Array[0] = 0;
        LOG_INFO("CMSIS_DAP_DetectDAPIDList: no DAP detected, wrote id[0]=0");
        return RDDI_SUCCESS;
    }

    for (size_t i = 0; i < count; i++) {
        DAP_ID_Array[i] = static_cast<int>(ctx->dapIdList[i]);
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_Commands(const RDDIHandle handle, int num,
                                   unsigned char **request, int *req_len,
                                   unsigned char **response, int *resp_len)
{
    LOG_DEBUG("CMSIS_DAP_Commands: enter, handle=%d, num=%d", handle, num);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (num != 1 || *req_len != 1 || *resp_len != 1) {
        return RDDI_BADARG;
    }

    uint8_t cmd = request[0][0];

    if (cmd != ID_DAP_RESET_TARGET) {
        return RDDI_BADARG;
    }

    // Handle reset target command
    LOG_INFO("CMSIS_DAP_Commands: 收到 ID_DAP_RESET_TARGET(0x0A)，执行目标复位");

    // ★ raw 返回值（**DEBUG** 级）：直接走统一分发点，命令字节与**完整响应**都打出来 ——
    //   最底层调试就在这里看，不必再去 HID 层对日志（HID 层也会留一份 "DAP raw"）。
    uint8_t cmdBuf[1] = { static_cast<uint8_t>(ID_DAP_RESET_TARGET) };
    uint8_t resp[8] = {};
    size_t respLen = sizeof(resp);
    const int rc = ORBMDK::ORBMDK_HID_DAPCommand(cmdBuf, sizeof(cmdBuf), resp, &respLen, ORBMDK_TIMEOUT_RESET_MS);
    LOG_DEBUG("CMSIS_DAP_Commands raw: cmd=0x%02X reqLen=1 -> rc=%d respLen=%u "
              "resp=[%02X %02X %02X %02X]",
             cmdBuf[0], rc, static_cast<unsigned>(respLen), resp[0], resp[1], resp[2], resp[3]);
    return rc == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWJ_Sequence(const RDDIHandle handle, int num,
                                       unsigned char *request)
{
    LOG_DEBUG("CMSIS_DAP_SWJ_Sequence: enter, handle=%d, num=%d", handle, num);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_SWJ_Sequence(static_cast<uint8_t>(num), request);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWJ_Pins(const RDDIHandle handle, unsigned char pinselect,
                                   unsigned char pinout, int *res, int wait)
{
    LOG_DEBUG("CMSIS_DAP_SWJ_Pins: enter, handle=%d, sel=0x%02X, out=0x%02X, wait=%d",
              handle, pinselect, pinout, wait);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_SWJ_Pins(pinselect, pinout, res, wait);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

// ============================================================================
// 缺失的 CMSIS-DAP 函数实现
// ============================================================================

RDDI_FUNC int CMSIS_DAP_Delay(const RDDIHandle handle, int delay_us)
{
    LOG_DEBUG("CMSIS_DAP_Delay: enter, handle=%d, delay_us=%d", handle, delay_us);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 发送 DAP_Delay 命令 (0x09)
    uint8_t cmd[3] = { 0x09, (uint8_t)(delay_us & 0xFF), (uint8_t)((delay_us >> 8) & 0xFF) };
    uint8_t resp[2] = {0};
    size_t respLen = sizeof(resp);
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
    return ret == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

RDDI_FUNC int CMSIS_DAP_ResetTarget(const RDDIHandle handle)
{
    LOG_DEBUG("CMSIS_DAP_ResetTarget: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    LOG_INFO("CMSIS_DAP_ResetTarget: 执行目标复位（DAP_ResetTarget）");
    int status = ORBMDK::DAP_ResetTarget();
    LOG_INFO("CMSIS_DAP_ResetTarget: DAP_ResetTarget -> status=%d (%s)",
             status, status == 0 ? "OK" : "FAIL");
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

/**
 * @brief 判断指针是否指向**可写**内存
 *
 * 用于挡住宿主（AGDI）传进来的非法"出参"指针。
 *
 * 背景（详见 COMPAT_ANALYSIS §15）：AGDI 给 `CMSIS_DAP_Connect` 的第 2 个实参
 * 并不是 `int*`，而是它在适配器对话框里保存的**选中项索引**：
 *     ifNo = 0 -> 实参是 NULL，被判空跳过 -> 一直相安无事
 *     ifNo = 1 -> 实参是 (int*)1 -> 向地址 1 写入 -> 0xc0000005，UV4 直接消失
 * 以前只有一个接口、索引恒为 0，所以这个坑几十年都没暴露；一旦把
 * V1/V2 两个接口都暴露出来，用户选了第二项就必然踩中。
 */
static bool IsWritablePointer(const void* p)
{
    if (!p) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
        return false;
    }
    const DWORD kWrite = PAGE_READWRITE | PAGE_WRITECOPY |
                         PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & kWrite) != 0 && mbi.RegionSize >= sizeof(int);
}

RDDI_FUNC int CMSIS_DAP_Connect(const RDDIHandle handle, int *connectedInterface)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_Connect: invalid handle");
        return RDDI_INVHANDLE;
    }

    // 第 2 个实参必须原样记录：AGDI 在"多接口"路径里传的不是 int*，而是它保存的
    // **选中项索引**（ifNo=0 -> NULL，ifNo=1 -> (int*)1）。这是判断"该值到底是
    // 出参指针还是接口序号"的唯一现场依据（见 COMPAT_ANALYSIS §15）。
    LOG_DEBUG("CMSIS_DAP_Connect: enter, handle=%d, arg2=%p (as index/ptr int=%d)",
              handle, (void *)connectedInterface, (int)(intptr_t)connectedInterface);
    LOG_DEBUG("CMSIS_DAP_Connect: starting SWD connection sequence");

    // 连接目标：**按 ConfigureInterface 里 Port= 的选择分岔**（§18.9）。
    //   SWD  : 沿用历史路径（port=1 + 主机侧补切换序列）
    //   JTAG : JtagInitSequence（port=2 + 下发 IR 长度 + TAP 复位 + 扫链）
    // 下面所有 SWD 专有步骤都被 `if (mode == 1)` 保护，JTAG 下自动跳过。
    int mode;
    if (ctx->isSWD) {
        mode = ORBMDK::DAP_ConnectTarget();
        LOG_DEBUG("CMSIS_DAP_Connect: DAP_ConnectTarget returned mode=%d", mode);
    } else {
        uint32_t jtagIdcode = 0;
        int jmode = 0;
        if (!JtagInitSequence(ctx, &jmode, &jtagIdcode)) {
            LOG_ERROR("CMSIS_DAP_Connect: JTAG 建链失败（检查目标是否物理接了 TDI/TDO、"
                      "以及 Keil 的 Port 是否确实选了 JTAG）");
            return RDDI_DAP_ERROR;
        }
        mode = DAP_CONNECT_JTAG;
    }
    if (mode < 0 || mode > 2) {
        LOG_ERROR("CMSIS_DAP_Connect: connect failed, mode=%d", mode);
        return RDDI_DAP_ERROR;
    }
    // 如果 mode=0，使用 SWD 作为默认
    if (mode == 0) {
        mode = 1;
        LOG_DEBUG("CMSIS_DAP_Connect: firmware returned default mode, using SWD");
    }
    LOG_DEBUG("CMSIS_DAP_Connect: DAP_ConnectTarget succeeded, mode=%s", mode == 1 ? "SWD" : "JTAG");

    // 手动发送 JTAG-to-SWD 切换序列（与 OpenOCD 的 cmsis_dap_swd_switch_seq 一致）。
    // 部分固件的 DAP_Connect(port=SWD) 内部切换序列不稳定（line reset 仅 50 周期），
    // 主机侧在 Connect 之后再补一次标准切换序列，可确保目标可靠进入 SWD 模式。
    // 标准序列：≥50 个 1 + 16-bit 0xE79E(LSB first) + ≥50 个 1 + idle。
    if (mode == 1) {
        uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };  // 56 位 1
        uint8_t jtagToSwd[] = { 0x9E, 0xE7 };                                // 16 位 0xE79E (LSB first)
        uint8_t idle[] = { 0x00 };
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(16, jtagToSwd);
        ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        ORBMDK::DAP_SWJ_Sequence(8, idle);
        LOG_DEBUG("CMSIS_DAP_Connect: sent JTAG-to-SWD switch sequence");
    }

    // 配置 SWD/JTAG 参数
    if (mode == 1) {
        // SWD 模式
        ORBMDK::DAP_ConfigureSWD(0);  // Default SWD config
        LOG_DEBUG("CMSIS_DAP_Connect: configured SWD");
    }

    // 配置传输参数 (超时重试次数)
    ORBMDK::DAP_ConfigureTransfer(0, kDefaultWaitRetry, kDefaultMatchRetry);
    LOG_DEBUG("CMSIS_DAP_Connect: configured transfer (wait=%d, match=%d)", kDefaultWaitRetry, kDefaultMatchRetry);

    // ------------------------------------------------------------------
    // SWD 建链收尾：必须做一次 **DPIDR 读**（本函数此前缺这一步）
    //
    // ADIv5 的 SWD 建链序列是：
    //     line reset → JTAG-to-SWD(0xE79E) → line reset → **读 DPIDR**
    // OpenOCD 的 `swd_connect` 最后一步也正是这次读。只做前四步时，DP 会停在
    // "刚切换完"的状态：后续**读**还能成功，而**写**会被目标拒绝
    // （Transfer Response = 0x07 / ACK=7=NO_ACK）。
    //
    // 实测（orbtrace + STM32F1，V1(HID) 与 V2(Bulk) 同一现象）：
    //   * 我们的最小宿主（test/swdprobe.cpp）在 Connect 之后立刻读 DPIDR
    //     → 稳定拿到 0x2BA01477；
    //   * Keil 的 AGDI 在 Connect 之后**第一条**访问是
    //     DAP_WriteReg(DP SELECT = 0xF0)（扫 ROM 表用）→ 直接 NO_ACK，
    //     再清 sticky、重试仍失败 → 报 "RDDI-DAP Error"。
    // 即：链路收尾是驱动（本层）的责任，不能等宿主去补一次读。
    // ------------------------------------------------------------------
    if (mode == 1) {
        uint32_t idcode = 0;
        int st = ORBMDK::DAP_Transfer(0, 0x02, &idcode);   // 0x02 = 读 DP 0x0 (DPIDR)

        if (st != ORBMDK::DAP_RES_OK || idcode == 0 || idcode == 0xFFFFFFFFu) {
            // 还没起来：补一次完整线复位 + 切换，再读一次
            LOG_WARN("CMSIS_DAP_Connect: DPIDR read failed (status=%d, idcode=0x%08X), "
                     "re-running the SWD switch sequence", st, idcode);
            uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            uint8_t jtagToSwd[] = { 0x9E, 0xE7 };
            uint8_t idle[] = { 0x00 };
            ORBMDK::DAP_SWJ_Sequence(56, lineReset);
            ORBMDK::DAP_SWJ_Sequence(16, jtagToSwd);
            ORBMDK::DAP_SWJ_Sequence(56, lineReset);
            ORBMDK::DAP_SWJ_Sequence(8, idle);
            st = ORBMDK::DAP_Transfer(0, 0x02, &idcode);
        }
        LOG_DEBUG("CMSIS_DAP_Connect: DPIDR read -> status=%d, idcode=0x%08X", st, idcode);

        // 清一次 DP 的 sticky/待处理错误：上一轮会话（或刚才那次失败）留下的
        // 错误位会让宿主的第一条**写**直接吃 FAULT；而 FAULT 之后的写会让固件
        // 提前结束数据相位，目标随即进入"协议错误/静默"，表现为后续全 NO_ACK。
        // 用专用 ABORT 命令(0x08)下发，它在 NO_ACK 状态下通常也能过。
        if (ORBMDK::DAP_WriteAbort(0, 0x1E) == 0) {
            LOG_DEBUG("CMSIS_DAP_Connect: DP ABORT written (0x1E, cleared sticky)");
        }
    }

    // 返回连接的接口类型
    //
    // ⚠️ 必须先校验指针再写：AGDI 传进来的不是 int*，而是"选中项索引"
    //    （ifNo=1 时就是 (int*)1）。不校验就会向地址 1 写入 → AV。
    //    见 IsWritablePointer 与 COMPAT_ANALYSIS §15。
    if (IsWritablePointer(connectedInterface)) {
        *connectedInterface = mode;  // 1=SWD, 2=JTAG
    } else if (connectedInterface) {
        // 不是故障：这是 AGDI 的固定行为（该参数传的是"选中项索引"，不是指针），
        // 我们按契约跳过写入即可。因此降到 DEBUG —— 之前用 WARN 会在**每次**
        // Connect 时刷一行，污染默认日志与 Keil 日志窗口（见 COMPAT_ANALYSIS §15）。
        LOG_DEBUG("CMSIS_DAP_Connect: out-pointer %p is not writable "
                  "(AGDI passes the selected interface index here) -> write skipped",
                  (void *)connectedInterface);
    }

    // 设置连接状态
    ctx->isConnected = true;
    LOG_DEBUG("CMSIS_DAP_Connect: connection complete, interface=%s", mode == 1 ? "SWD" : "JTAG");

    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// CMSIS_DAP_Disconnect —— AGDI 的 RDDI 绑定表里**唯一**此前没实现的那个
//
// 反汇编 CMSIS_AGDI.dll 的"绑定 RDDI 函数表"例程（0x1002C1B0）可知：它在最外层
// 流程（0x1002244C）先 LoadLibrary("<AGDI目录>\CMSIS_DAP.dll")，然后按固定顺序
// GetProcAddress 共 54 个导出名，逐个存进 0x10362F08 起的函数指针槽
// （索引 i -> 0x10362F08 + i*4）。其中 54 个名字里本层只缺这一个。
//
// 缺了不会让绑定例程本身失败（它对槽位不做 NULL 校验），但该槽恒为 NULL：
// AGDI 一旦在"断开/收尾"路径上调用它就是踩空指针。补上即可。
// ---------------------------------------------------------------------------
RDDI_FUNC int CMSIS_DAP_Disconnect(const RDDIHandle handle)
{
    LOG_DEBUG("CMSIS_DAP_Disconnect: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_Disconnect: invalid handle");
        return RDDI_INVHANDLE;
    }

    // 幂等：AGDI 可能在错误收尾路径上重复调用，未连接时不应再发断开指令
    if (ctx->isConnected) {
        ORBMDK::DAP_DisconnectTarget();
        ctx->isConnected = false;
    }

    // 状态 LED 熄灭（与 RDDI_Close 一致：官方 AGDI 不驱动 DAP_HostStatus）
    SetHostLed(ctx, kHostLedConnect, false);
    SetHostLed(ctx, kHostLedRunning, false);

    LOG_DEBUG("CMSIS_DAP_Disconnect: target released");
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_SWJ_Clock(const RDDIHandle handle, unsigned int clock)
{
    LOG_DEBUG("CMSIS_DAP_SWJ_Clock: enter, handle=%d, clock=%u", handle, clock);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    ctx->debugClock = clock;
    int status = ORBMDK::DAP_SetSWJClock(clock);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_WriteABORT(const RDDIHandle handle, int dap_id, unsigned int abort)
{
    LOG_DEBUG("CMSIS_DAP_WriteABORT: enter, handle=%d, dap_id=%d, abort=0x%08X", handle, dap_id, abort);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!ctx->isSWD) {
        // JTAG：ABORT 是 IR=ABORT 的 DR（RnW=A2=A3=0 + 32 位数据），ACK 忽略
        uint8_t ack = 0;
        uint32_t dummy = 0;
        const bool ok = JtagSetIr(ctx, kJtagIrAbort) &&
                        JtagDrScan(ctx, 0x00, abort, &dummy, &ack);
        ctx->jtagCurIr = 0;
        if (ok) {
            ctx->jtagWritePending = true;
            JtagDapFlush(ctx);
        }
        return ok ? RDDI_SUCCESS : RDDI_DAP_ERROR;
    }

    int status = ORBMDK::DAP_WriteAbort(dap_id, abort);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWD_Configure(const RDDIHandle handle, uint8_t cfg)
{
    LOG_DEBUG("CMSIS_DAP_SWD_Configure: enter, handle=%d, cfg=0x%02X", handle, cfg);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_ConfigureSWD(cfg);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWD_Sequence(const RDDIHandle handle, int num, unsigned char *request)
{
    LOG_DEBUG("CMSIS_DAP_SWD_Sequence: enter, handle=%d, num=%d", handle, num);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // SWD_Sequence 命令 (0x1D)
    // 格式: [0x1D] [count] [data...]
    // count 为 uint8_t，最多 255 位 = 32 字节，2 + 32 = 34 < 64
    if (num < 0 || num > 255) {
        return RDDI_BADARG;
    }
    uint8_t cmd[64] = { ID_DAP_SWD_SEQUENCE, (uint8_t)num };
    size_t dataBytes = (size_t)((num + 7) / 8);
    memcpy(cmd + 2, request, dataBytes);

    // 响应（含报告ID）：[报告ID][命令ID][Status]
    uint8_t resp[64] = {0};
    size_t respLen = sizeof(resp);
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, 2 + dataBytes, resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
    // 原实现检查 resp[0]==1，但 resp[0] 是报告ID(0x00)，导致恒为失败。
    if (ret != 0) return RDDI_FAILED;
    if (respLen >= 3 && resp[1] == ID_DAP_SWD_SEQUENCE && resp[2] != 0) return RDDI_FAILED;
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_JTAG_Configure(const RDDIHandle handle, int count, uint8_t *ir_len)
{
    LOG_DEBUG("CMSIS_DAP_JTAG_Configure: enter, handle=%d, count=%d", handle, count);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 宿主显式下发 IR 长度的入口（ARM rddi_dap.h 的 CMSIS_DAP_JTAG_Configure）。
    // 本层自己要用的那份在 JtagInitSequence 里，两者走同一个 DAP 层实现。
    if (!ir_len || count <= 0 || count > 16) {
        return RDDI_BADARG;
    }

    // ⚠ 超出固件链容量时**不下发**：irlenx = 6 x 5 bit、dev = 3 bit，
    //   硬发会被静默截断（IR 按 5 bit / 器件数溢出）→ 走链错 → 通道静默变哑。
    //   本层引擎走 ID_DAP_JTAG_SEQUENCE，不依赖固件这份布局，故只丢一次同步。
    int status = 0;
    if (!JtagChainFitsFirmware(count, ir_len)) {
        LOG_WARN("CMSIS_DAP_JTAG_Configure: count=%d 超出固件链容量（<= %d 器件且单器件 IR <= %d bit）"
                 "-> 不下发 ID_DAP_JTAG_CONFIGURE（本层引擎不依赖它）",
                 count, kJtagFwMaxDevices, kJtagFwMaxIrBits);
    } else {
        status = ORBMDK::DAP_JTAG_Configure(ir_len, (uint8_t)count);
    }
    if (status == 0) {
        // 记下来，供 CMSIS_DAP_JTAG_GetIRLengths 回给宿主，同时作为本层引擎的链布局。
        // ir_len[0] = 离 TDO 最近的器件（与固件 JTAG_DP.c 的约定一致）。
        const int n = (count > 8) ? 8 : count;
        ctx->jtagDevCount    = n;
        ctx->jtagChainCount  = n;
        for (int i = 0; i < 8; ++i) {
            ctx->jtagIrLens[i] = (i < n) ? ir_len[i] : 0;
        }
        ctx->jtagIrLength    = ir_len[0];
        ctx->jtagChainIrBits = 0;
        for (int i = 0; i < n; ++i) ctx->jtagChainIrBits += ctx->jtagIrLens[i];
        // 未扫过链时无法定位 DP：先按位置 0 处理（JtagInitSequence 会按 IDCODE 自动判定）
        if (ctx->jtagDpIndex < 0 || ctx->jtagDpIndex >= n) ctx->jtagDpIndex = 0;
        ctx->jtagCurIr = 0;
    }
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_JTAG_Sequence(const RDDIHandle handle, int num, uint8_t *info,
                                       uint8_t *tdi, uint8_t *tdo, uint8_t mask)
{
    LOG_DEBUG("CMSIS_DAP_JTAG_Sequence: enter, handle=%d, num=%d, info=%p, tdi=%p, tdo=%p",
              handle, num, (void *)info, (void *)tdi, (void *)tdo);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 宿主 API 语义（ARM rddi_dap.h 的 CMSIS_DAP_JTAG_Sequence）：
    //   num  = 段数；info[i] = 位数(1..63；0 表示 64) | 0x40 = TMS 电平 | 0x80 = 捕获 TDO；
    //   tdi  = 各段 TDI 依次拼接（每段 (bits+7)/8 字节，LSB first）；tdo = 各捕获段拼接。
    // 与本层 HID 接口语义一致，逐段拆开后下发。
    if (num <= 0 || num > 64 || !info) {
        return RDDI_BADARG;
    }

    std::vector<ORBMDK::JtagSeg> segs;
    segs.reserve(static_cast<size_t>(num));
    const uint8_t* tdiCur = tdi;
    size_t tdoCap = 0;
    for (int i = 0; i < num; ++i) {
        const int bits = (info[i] & 0x3F) ? (info[i] & 0x3F) : 64;
        ORBMDK::JtagSeg s;
        s.bits    = static_cast<uint8_t>(bits);
        s.tms     = (info[i] & 0x40) ? 1 : 0;
        s.capture = (info[i] & 0x80) ? 1 : 0;
        s.tdi     = tdiCur;
        segs.push_back(s);
        if (tdiCur) tdiCur += (bits + 7) / 8;
        if (s.capture) tdoCap += static_cast<size_t>((bits + 7) / 8);
    }

    size_t tdoLen = 0;
    const int status = ORBMDK::DAP_JTAG_Sequence(segs.data(), static_cast<int>(segs.size()),
                                                 tdo, tdo ? tdoCap : 0, &tdoLen);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_JTAG_GetIDCODEs(const RDDIHandle handle, int *count, uint32_t *idcodes)
{
    LOG_DEBUG("CMSIS_DAP_JTAG_GetIDCODEs: enter, handle=%d, count=%p, idcodes=%p",
              handle, (void *)count, (void *)idcodes);
    // 输出参数必须先写入 —— 这里尤其关键：
    // AGDI 把 *count 当作"**设备数量**"使用（反汇编 0x1002C918 → 0x1002C91F，
    // 位于设备探测例程 0x1002c890，其出参直接决定 Debug Settings 里
    // SW Device 列表显示几行），而且只看输出值、不看返回值。
    //
    // 历史问题：旧实现只在 DAP_JTAG_IDCODE 成功时才写 *count。设备拔出时
    // USB 必然失败 → 一个字节都不写 → AGDI 读到它自己栈上的残留值（>0）
    // → 认为有设备 → 列表仍显示一行 "IDCODE 0x00000000 / Unknown device"。
    if (count) {
        *count = 0;
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    // ---------------------------------------------------------------------
    // 先按"协议无关的 DAP 扫描"回答：用与 CMSIS_DAP_GetDeviceIDList /
    // DetectDAPIDList / DetectNumberOfDAPs **同一张表**（ctx->dapIdList）。
    //
    // 关键认知（COMPAT_ANALYSIS §4.14）：AGDI 在 **SWD 模式**下同样会调用这个
    // 名字带 JTAG_ 的函数 —— 它是协议无关的"扫描 DAP / 取 IDCODE"探测器。
    // 旧实现只认固件的 ID_DAP_JTAG_IDCODE 命令：SWD 目标上那条命令必然失败，
    // 于是即使目标就在那儿、IDCODE 也能读出来，这里仍然一个 IDCODE 都给不出。
    // 单设备路径下 AGDI 只取 *count（还能容忍 0），但在它的"多 DAP"分支里
    // 是要拿 idcodes 列表的 —— 列表空 = 失败。
    //
    // 注：官方按 4 参调用（handle, count, idcodes, sizeOfArray）。本函数仍按
    // 3 参实现：x86 __cdecl 由调用方清栈，多传的实参不会破坏调用，也不会被
    // 本函数读取（因此不能拿它当容量上限用）。表里至多 kSingleDapCount(1) 项。
    // ---------------------------------------------------------------------
    if (ctx->dapIdList.empty()) {
        DetectTargetDapIdList(ctx, nullptr, nullptr);
    }

    if (!ctx->dapIdList.empty()) {
        int n = 0;
        for (size_t i = 0; i < ctx->dapIdList.size() && i < 4; ++i) {
            if (idcodes) {
                idcodes[i] = ctx->dapIdList[i];
            }
            ++n;
        }
        *count = n;
        return RDDI_SUCCESS;
    }

    // 目标未探测到时，仍试一次固件的 JTAG 扫描（JTAG 链路下才有效）
    int idcodeCount = 0;
    int status = ORBMDK::DAP_JTAG_IDCODE(&idcodeCount, idcodes);
    if (status == 0) {
        *count = idcodeCount;
    } else {
        // "没扫到 IDCODE"不是错误，必须返回成功。
        //
        // AGDI 把本函数的返回值当**硬失败**处理（反汇编 0x1002C935：
        // 非 0 → 0x1002C9D8 → 返回 0x2028）。一旦报错，AGDI 就不会更新
        // JTAG_devs，对话框继续显示上一次的值（即工程里保存的
        // "-D00(00000000) -N00(Unknown device)"），表现为"目标不在却仍有一行"。
        // 返回成功 + count = 0，AGDI 才会把设备数清零、列表变空。
        LOG_INFO("CMSIS_DAP_JTAG_GetIDCODEs: no IDCODE found (status=%d), count=0", status);
    }
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_JTAG_GetIRLengths(const RDDIHandle handle, int *count, uint8_t *lengths)
{
    LOG_DEBUG("CMSIS_DAP_JTAG_GetIRLengths: enter, handle=%d", handle);
    // 同 GetIDCODEs：先无条件写输出参数，避免调用方读到栈残留。
    if (count) {
        *count = 0;
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // IR 长度由 JtagInitSequence 下发给探针并记录在 ctx（§18.9）。
    // 宿主（AGDI）必须拿到它才能自己生成 JTAG_Sequence；链上单器件时就是 4。
    //
    // ⚠️ SWD 模式必须报 0：否则宿主会以为链上存在 JTAG 器件（纯 SWD 会话下
    // 不该出现的信息）。jtagDevCount 只在 JTAG 建链 / DAP_JTAG_Configure 成功时置非 0。
    if (ctx->isSWD) {
        LOG_DEBUG("CMSIS_DAP_JTAG_GetIRLengths: SWD mode -> count=0");
        return RDDI_SUCCESS;   // *count 已在函数开头写入 0
    }
    if (count) {
        *count = ctx->jtagDevCount;
    }
    if (lengths && ctx->jtagDevCount > 0) {
        // 逐器件回报（irLens[0] = 离 TDO 最近的器件）。
        // 旧实现把首器件的长度复制给所有器件 —— 多 TAP 链（本目标有 2 个 TAP：
        // IR=4 的 JTAG-DP + IR=5 的边界扫描）会因此配错，扫链/IDCODE 全错。
        for (int i = 0; i < ctx->jtagDevCount && i < 8; ++i) {
            lengths[i] = ctx->jtagIrLens[i];
        }
    }
    return RDDI_SUCCESS;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Control(handle, int control)
//   control: 0 = Stop, 1 = Start
RDDI_FUNC int CMSIS_DAP_SWO_Control(const RDDIHandle handle, int control)
{
    LOG_DEBUG("CMSIS_DAP_SWO_Control: enter, handle=%d, control=%d", handle, control);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // ---- Read 路径：Start 之前必须把 SWO 的三个基本配置补齐 ----
    // 走 Read 时，AGDI 只会调 SWO_Control / SWO_Baudrate / SWO_Data 这几个函数，
    // 它**不会**自己下发 SWO 的 Transport 与 Mode。也就是说"从哪个引脚收数据、
    // 用 UART 还是 Manchester 编码"这件事，AGDI 只在 ConfigureDebugger 的配置串里
    // 告诉过本层一次（Trace=…），如果本层不在这里补发，探针就只能用它自己的默认值跑，
    // 表现就是：界面里选了 Manchester，实际收到的还是按 UART 解出来的乱码。
    //
    // 失败只记 WARN、不阻断 Start：老固件可能不支持其中某条命令，
    // 那种情况下维持"和以前一样"比直接拒绝启动更可取。
    if (control != 0) {
        const uint8_t port = (ctx->swoPortMode == 2) ? 2 : 1;   // 2 = Manchester, 1 = UART

        if (ORBMDK::DAP_SWO_Transport(1) != 0) {   // 1 = 使用 SWO 引脚（UART / Manchester 都走这一条）
            LOG_WARN("CMSIS_DAP_SWO_Control: DAP_SWO_Transport(1) 失败（固件可能不支持，忽略）");
        }
        if (ORBMDK::DAP_SWO_Mode(port) != 0) {
            LOG_WARN("CMSIS_DAP_SWO_Control: DAP_SWO_Mode(%u) 失败（固件可能不支持，忽略）", port);
        }
        if (ctx->swoBaudrate > 0 &&
            ORBMDK::DAP_SWO_Baudrate(static_cast<uint32_t>(ctx->swoBaudrate)) != 0) {
            LOG_WARN("CMSIS_DAP_SWO_Control: DAP_SWO_Baudrate(%d) 失败（固件可能不支持，忽略）",
                     ctx->swoBaudrate);
        }

        LOG_INFO("CMSIS_DAP_SWO_Control: Start 前置下发 port=%u(%s), baud=%d",
                 port, port == 2 ? "Manchester" : "UART", ctx->swoBaudrate);
    }

    uint8_t status = 0;
    int ret = ORBMDK::DAP_SWO_Control(static_cast<uint8_t>(control), &status);
    return ret == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Status(handle, int *count, int *status)
//   count : Trace Buffer 中尚未读取的字节数
//   status: Bit0 = Trace Capture(1 活动/0 停止), Bit6 = Stream Error, Bit7 = Buffer Overrun
// 旧签名只有 status 一个输出参数，按标准调用时 count 指针会被当成 status 写入，
// 属于内存破坏级错误。
RDDI_FUNC int CMSIS_DAP_SWO_Status(const RDDIHandle handle, int *count, int *status)
{
    LOG_DEBUG("CMSIS_DAP_SWO_Status: enter, handle=%d, count=%p, status=%p",
              handle, (void *)count, (void *)status);
    if (count)  *count  = 0;
    if (status) *status = 0;

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!status) {
        return RDDI_BADARG;
    }

    // 发送 SWO_Status 命令 (0x1B)
    // 响应（含报告ID）：[报告ID][命令ID][Status]（探针带计数时后面还有 [Count_L][Count_H]）
    uint8_t cmd[1] = { ID_DAP_SWO_STATUS };
    uint8_t resp[8] = {0};
    size_t respLen = sizeof(resp);
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);

    if (ret != 0 || respLen < 3 || resp[1] != ID_DAP_SWO_STATUS) {
        return RDDI_FAILED;
    }

    *status = resp[2];

    // 待读字节数：本层不做本地缓存（Read 路径的数据是按需从探针拉的），
    // 所以这个数只能取自探针响应里的计数字段。此前一律用 swoBuffer.size()，
    // 而 swoBuffer 在 Read 路径下从不被填，等于永远报 0 —— AGDI 会据此认为
    // "没东西可读"，表现出来就是 Trace 窗口一直空着。固件不回计数字段时
    // 仍退回原来的 0，行为与以前完全一致。
    int pending = static_cast<int>(ctx->swoBuffer.size());
    if (respLen >= 5) {
        pending = static_cast<int>(resp[3] | (static_cast<int>(resp[4]) << 8));
    }
    if (count) {
        *count = pending;
    }
    return RDDI_SUCCESS;
}

// CMSIS_DAP_SWO_Baudrate(handle, int *baudrate)
//
// 这条路径是 µVision 的 **SWO 时钟自动探测**：AGDI 把 SWO 时钟频率（全局
// [0x1021268C]）逐次除以候选分频 (div+1)，结果写到栈上，再把**地址**交给本函数
// 下发；返回 0 表示"该分频可用"，非 0 表示"换下一个再试"。div 从 0 试到 0x1FFF，
// 全部失败 → 界面报 "SWO CLOCK not support"。
//
// ⚠️ 两条硬约束（均来自 AGDI 反汇编，改动前务必重读 Todo.md §18.15）：
//   1) 第二参数是**指针**。按值接收会把栈地址（约 1.7 MB 量级的数）当成波特率
//      下发，探针给出的结果必然对不上，界面随即报 "SWO CLOCK not support"；
//   2) **不要回写 *baudrate**。包装函数 0x1003D030 先 `mov ebx,[edi]` 存下原值，
//      调用后再 `cmp [edi], ebx`；一旦发现值被改写就返回 0x2022（非 0），
//      调用方据此判为"该分频不可用" —— 回写反而会让整轮探测失败。
RDDI_FUNC int CMSIS_DAP_SWO_Baudrate(const RDDIHandle handle, int *baudrate)
{
    LOG_DEBUG("CMSIS_DAP_SWO_Baudrate: enter, handle=%d, baudrate=%p", handle, (void *)baudrate);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!baudrate || *baudrate <= 0) {
        return RDDI_BADARG;
    }

    const uint32_t baud = static_cast<uint32_t>(*baudrate);
    const int status = ORBMDK::DAP_SWO_Baudrate(baud);

    if (status == 0) {
        // 探测最多连调 8192 次，正常只有一次成功，值得单独记一条 INFO。
        ctx->swoBaudrate = static_cast<int>(baud);
        LOG_INFO("CMSIS_DAP_SWO_Baudrate: 探针接受 %u Hz，SWO 时钟探测结束", baud);
        return RDDI_SUCCESS;
    }

    // 失败往往成百上千次，前几条就足够说明"探针是否接受这个命令"。
    if (ctx->swoBaudFailCount < 4) {
        LOG_WARN("CMSIS_DAP_SWO_Baudrate: 探针拒绝 %u Hz（第 %d 次失败；"
                 "连续大量失败即界面报 SWO CLOCK not support 的原因）",
                 baud, ctx->swoBaudFailCount + 1);
    }
    ++ctx->swoBaudFailCount;
    return RDDI_DAP_ERROR;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Data(handle, int *num_written, void *buffer, int *status)
//   num_written: [in] buffer 容量，[out] 实际写入字节数
// 旧签名缺少 status 输出参数。
RDDI_FUNC int CMSIS_DAP_SWO_Data(const RDDIHandle handle, int *num_written,
                                 void *buffer, int *status)
{
    LOG_DEBUG("CMSIS_DAP_SWO_Data: enter, handle=%d, num_written=%p, buffer=%p",
              handle, (void *)num_written, buffer);
    if (num_written) *num_written = 0;
    if (status)      *status      = 0;

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!num_written || !buffer) {
        return RDDI_BADARG;
    }

    uint8_t swoStatus = 0;
    size_t dataLen = static_cast<size_t>(*num_written);
    int ret = ORBMDK::DAP_SWO_Data(static_cast<uint8_t *>(buffer), &dataLen, &swoStatus, NULL);

    *num_written = static_cast<int>(dataLen);
    if (status) {
        *status = swoStatus;
    }
    return ret == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

// 签名与 ARM rddi_dap_cmsis.h 对齐：CMSIS_DAP_GetGUID(handle, ifNo, str, len)
// 历史问题：此处曾缺少 ifNo 参数，按标准签名调用时参数会整体错位，
//           guid 实际收到的是 ifNo（0/1），导致向非法地址写入。
RDDI_FUNC int CMSIS_DAP_GetGUID(const RDDIHandle handle, int ifNo, char *guid, int len)
{
    LOG_DEBUG("CMSIS_DAP_GetGUID: enter, handle=%d, ifNo=%d, len=%d", handle, ifNo, len);
    if (guid && len > 0) {
        guid[0] = '\0';  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!guid || len <= 0) {
        return RDDI_BADARG;
    }

    // CMSIS-DAP 不提供真正的 GUID，用设备标识拼一个稳定字符串。
    snprintf(guid, (size_t)len, "ORBTrace-%s",
             ctx->serialNumber.empty() ? "Unknown" : ctx->serialNumber.c_str());
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：CMSIS_DAP_GetInterfaceVersion
//
// 官方 CMSIS_DAP.dll 反汇编（RVA 0x147A0）：
//     mov ecx,[ebp+0Ch]        ; arg2 是指针
//     mov dword ptr [ecx],20000h
// 即 2 个参数，arg2 是 **int\***，返回版本号 0x00020000
// （bits[31:24]=major, [23:16]=minor, [15:0]=build → 0.2.0，与
//   RDDI_DAP_MAJOR_VERSION / RDDI_DAP_MINOR_VERSION 一致）。
//
// 旧版本声明为 (handle, char *version, int len) 并做 snprintf：
// 按官方 2 参调用时 len 取到栈上垃圾值，snprintf 会越界写 -> 崩溃。
// ---------------------------------------------------------------------------
RDDI_FUNC int CMSIS_DAP_GetInterfaceVersion(const RDDIHandle handle, int *version)
{
    LOG_DEBUG("CMSIS_DAP_GetInterfaceVersion: enter, handle=%d", handle);
    if (version) {
        *version = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!version) {
        return RDDI_BADARG;
    }

    *version = 0x00020000;  // major 0, minor 2, build 0
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_GetNumberOfDevices(const RDDIHandle handle, int *count)
{
    LOG_DEBUG("CMSIS_DAP_GetNumberOfDevices: enter, handle=%d, count=%p", handle, (void *)count);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    // 与 CMSIS_DAP_DetectNumberOfDevices 同一语义：返回"目标上扫描到的设备数"。
    // 官方 AGDI 的设备扫描例程把它当设备数使用（反汇编 0x1002C9BA →
    // CMSIS_DAP_GetNumberOfDevices(handle, &count)），返回固定 1 会让
    // "目标不在"时仍显示一行。
    //
    // 与 §4.6 #16 的关系：当年改为固定 1，是因为 dapIdList 从未被填充、
    // 此处恒返回 0，导致 "No Debug Unit Found"。现在列表为空时会主动探测
    // （DetectTargetDapIdList），因此可以安全地返回真实数量。
    if (ctx->dapIdList.empty()) {
        DetectTargetDapIdList(ctx, nullptr, nullptr);
    }

    *count = static_cast<int>(ctx->dapIdList.size());
    return RDDI_SUCCESS;
}

// CMSIS_DAP_ResetDAP - 重置 DAP 调试器
RDDI_FUNC int CMSIS_DAP_ResetDAP(const RDDIHandle handle)
{
    LOG_DEBUG("CMSIS_DAP_ResetDAP: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_ResetDAP: invalid handle");
        return RDDI_INVHANDLE;
    }

    LOG_DEBUG("CMSIS_DAP_ResetDAP: starting DAP reset sequence");

    // 执行 DAP 复位序列
    // 1. 重新连接目标 —— 按 Port= 的选择分岔（§18.9）
    int mode;
    if (ctx->isSWD) {
        mode = ORBMDK::DAP_ConnectTarget();
    } else {
        int jmode = 0;
        uint32_t jid = 0;
        if (!JtagInitSequence(ctx, &jmode, &jid)) {
            LOG_ERROR("CMSIS_DAP_ResetDAP: JTAG 建链失败");
            return RDDI_DAP_ERROR;
        }
        mode = DAP_CONNECT_JTAG;
    }
    if (mode < 0 || mode > 2) {
        LOG_ERROR("CMSIS_DAP_ResetDAP: connect failed, mode=%d", mode);
        return RDDI_DAP_ERROR;
    }
    if (mode == 0) mode = 1;  // 使用 SWD 作为默认
    LOG_DEBUG("CMSIS_DAP_ResetDAP: target connected, mode=%s", mode == 1 ? "SWD" : "JTAG");

    // 2. 手动发送 JTAG-to-SWD 切换序列（与 CMSIS_DAP_Connect 一致，需在 Connect 之后）
    int status = 0;
    if (mode == 1) {
        uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        uint8_t jtagToSwd[] = { 0x9E, 0xE7 };
        uint8_t idle[] = { 0x00 };
        status |= ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        status |= ORBMDK::DAP_SWJ_Sequence(16, jtagToSwd);
        status |= ORBMDK::DAP_SWJ_Sequence(56, lineReset);
        status |= ORBMDK::DAP_SWJ_Sequence(8, idle);
        if (status != 0) {
            LOG_ERROR("CMSIS_DAP_ResetDAP: SWJ_Sequence switch failed, status=%d", status);
            return RDDI_DAP_ERROR;
        }
        LOG_DEBUG("CMSIS_DAP_ResetDAP: sent JTAG-to-SWD switch sequence");
    }

    // 3. 配置传输参数（SWD_Configure 是 SWD 专有命令，JTAG 下不发）
    if (mode == 1) {
        ORBMDK::DAP_ConfigureSWD(0);
    }
    ORBMDK::DAP_ConfigureTransfer(0, kDefaultWaitRetry, kDefaultMatchRetry);

    // 4. 重置目标
    LOG_INFO("CMSIS_DAP_ResetDAP: 执行目标复位（DAP_ResetTarget）");
    status = ORBMDK::DAP_ResetTarget();
    if (status != 0) {
        LOG_ERROR("CMSIS_DAP_ResetDAP: DAP_ResetTarget failed, status=%d", status);
        return RDDI_DAP_ERROR;
    }

    ctx->isConnected = true;
    LOG_DEBUG("CMSIS_DAP_ResetDAP: DAP reset complete");

    return RDDI_SUCCESS;
}

// CMSIS_DAP_DetectNumberOfDevices - 检测可用设备数量
//
// 语义是"**目标上**扫描到的 DAP 数量"，不是"适配器是否已打开"。
// 官方 AGDI 把该值直接当作 JTAG_devs.cnt 使用（反汇编 0x1002CC6D → 0x1002CC9C），
// 决定 Debug Settings 里 SW Device 列表显示几行：
//   返回 1 → 显示一行 "IDCODE 0x00000000 / Unknown device"
//   返回 0 → 列表为空（设备未连接时应有的表现）
//
// 历史问题：旧实现用 ctx->initialized 判定，而 RDDI_Open 之后该标志恒为 true，
// 于是拔掉设备后列表仍显示一行（IDCODE 先是 0x4A57533B 栈残留，见 4.11；
// 补写 0 之后变成 0x00000000）。
RDDI_FUNC int CMSIS_DAP_DetectNumberOfDevices(const RDDIHandle handle, int *count)
{
    LOG_DEBUG("CMSIS_DAP_DetectNumberOfDevices: enter, handle=%d, count=%p", handle, (void *)count);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_DetectNumberOfDevices: invalid handle");
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    // AGDI 通常已先调用过 GetDeviceIDList（其中会触发探测），这里只是保证
    // 调用顺序无关：列表为空时主动探测一次。
    if (ctx->dapIdList.empty()) {
        DetectTargetDapIdList(ctx, nullptr, nullptr);
    }

    *count = static_cast<int>(ctx->dapIdList.size());
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：CMSIS_DAP_GetDeviceIDList
//
// 官方 CMSIS_DAP.dll 反汇编（RVA 0x14FD0）：
//     mov  eax,[ebp+0Ch]        ; arg2 = 目标数组指针
//     test eax,eax / je -> 返回 0x0D
//     mov  esi,[ebp+10h]        ; arg3
//     shr  esi,2                ; arg3 是 **字节数**，/4 得到元素个数
//     ...
//     mov  [eax],edi            ; 按 DWORD 逐个写入
// 即 3 个参数：int CMSIS_DAP_GetDeviceIDList(handle, int *idArray, size_t sizeOfArray)
//
// 旧版本声明为 (handle, int *count, char *deviceIDs, int len)，把第 2 个参数
// 当输出 count、第 3 个当 char* 缓冲区。Keil 实际传入的 arg3 是字节数（例如 0x20），
// 于是 deviceIDs[0]='\0' 变成向地址 0x20 写入 -> UV4.exe 崩溃
// （WER: CMSIS_DAP.dll, c0000005, 偏移 0xa1bc）。
// ---------------------------------------------------------------------------
RDDI_FUNC int CMSIS_DAP_GetDeviceIDList(const RDDIHandle handle, int *idArray, size_t sizeOfArray)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_GetDeviceIDList: invalid handle");
        return RDDI_INVHANDLE;
    }

    if (!idArray) {
        // 静默返回 RDDI_BADARG 会让现场完全看不见这次调用 —— 加日志。
        // （官方实现在 idArray == NULL 时也返回 0x0D，见 COMPAT_ANALYSIS §10.7）
        LOG_WARN("CMSIS_DAP_GetDeviceIDList: idArray == NULL -> RDDI_BADARG "
                 "(sizeOfArray=%llu)", (unsigned long long)sizeOfArray);
        return RDDI_BADARG;
    }

    // sizeOfArray 是字节数，可容纳的元素个数按 sizeof(int) 折算
    const int maxEntries = static_cast<int>(sizeOfArray / sizeof(int));
    if (maxEntries <= 0) {
        LOG_WARN("CMSIS_DAP_GetDeviceIDList: sizeOfArray=%llu -> nothing fits, "
                 "returning SUCCESS without writing", (unsigned long long)sizeOfArray);
        return RDDI_SUCCESS;
    }

    LOG_DEBUG("CMSIS_DAP_GetDeviceIDList: enter, handle=%d, array=%p, sizeOfArray=%llu, "
              "maxEntries=%d", handle, (void *)idArray,
              (unsigned long long)sizeOfArray, maxEntries);

    // 输出数组里必须是 **IDCODE**，不是 DAP 索引（见 DetectTargetDapIdList 的说明）。
    // 目标尚未探测过时主动探测一次。
    if (ctx->dapIdList.empty()) {
        DetectTargetDapIdList(ctx, nullptr, nullptr);
    }

    int n = static_cast<int>(ctx->dapIdList.size());
    if (n > maxEntries) {
        n = maxEntries;
    }

    if (n == 0) {
        // 未探测到目标（例如调试器已拔出）时**必须**写入，不能直接返回。
        // AGDI 的接收数组是它自己的栈局部变量，只看输出值、不看返回值：
        // 一个字节都不写的话，它会读到上一轮调用残留的栈内容 ——
        // 实测设备拔掉时对话框显示 "IDCODE 0x4A57533B"，按小端拆开是
        // ASCII ";SWJ"，正是上一轮 ConfigureInterface 的 cfg 串片段。
        // （与 CMSIS_DAP_DetectDAPIDList 的兜底保持一致，见 4.6 #17/#18）
        idArray[0] = 0;
        LOG_INFO("CMSIS_DAP_GetDeviceIDList: no device detected, wrote id[0]=0");
        return RDDI_SUCCESS;
    }

    for (int i = 0; i < n; i++) {
        idArray[i] = static_cast<int>(ctx->dapIdList[i]);
    }

    return RDDI_SUCCESS;
}

// DAP_GetSupportedHostStatusIDs - 获取支持的主机状态 ID
RDDI_FUNC int DAP_GetSupportedHostStatusIDs(const RDDIHandle handle, int *count, int *statusIDs)
{
    LOG_DEBUG("DAP_GetSupportedHostStatusIDs: enter, handle=%d, count=%p, statusIDs=%p",
              handle, (void *)count, (void *)statusIDs);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("DAP_GetSupportedHostStatusIDs: invalid handle");
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    // CMSIS-DAP 支持的主机状态类型
    // 0x00: 捕获状态, 0x01: 连接状态
    if (statusIDs) {
        statusIDs[0] = 0x00;  // 捕获状态
        statusIDs[1] = 0x01;  // 连接状态
    }
    *count = 2;
    LOG_DEBUG("DAP_GetSupportedHostStatusIDs: count=2, IDs=[0x00, 0x01]");
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// 配置串取值：AGDI 交来的是 `Key=Value;Key=Value;…` 形式的一条串。
// 取某个键的值（读到 ';' 或串尾为止），找不到就返回 false。
// 传进来的键必须带 '='（例如 "Trace="），这样 "Trace=" 不会误命中 "TraceBaudrate="。
// ---------------------------------------------------------------------------
static bool GetConfigValue(const char* config, const char* keyEq, char* out, size_t outLen)
{
    if (!config || !keyEq || !out || outLen == 0) {
        return false;
    }
    out[0] = '\0';

    const char* p = strstr(config, keyEq);
    if (!p) {
        return false;
    }
    p += strlen(keyEq);

    size_t n = 0;
    while (p[n] != '\0' && p[n] != ';' && (n + 1) < outLen) {
        out[n] = p[n];
        ++n;
    }
    out[n] = '\0';
    return true;
}

RDDI_FUNC int CMSIS_DAP_ConfigureDebugger(const RDDIHandle handle, const char *config)
{
    LOG_DEBUG("CMSIS_DAP_ConfigureDebugger: enter, handle=%d, config=%p", handle, (const void *)config);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (config) {
        // 解析调试器配置字符串
        ctx->lastErrorStr = config;
        LOG_INFO("CMSIS_DAP_ConfigureDebugger: config=\"%s\"", config);

        // --- Trace= ：用户在 Trace 页里选的端口 ---
        // AGDI 依 caps 的 0x04 / 0x08 决定界面上能选哪些端口（反汇编见 Todo.md §18.8），
        // 选中之后写进这条串。本层若不解析，"选了 Manchester" 会被悄悄当作 UART 跑，
        // 表现为 trace 窗口时有时无或全是乱码。
        char port[64] = {};
        if (GetConfigValue(config, "Trace=", port, sizeof(port))) {
            if (_stricmp(port, "SWO-UART") == 0) {
                ctx->swoPortMode = 1;
            } else if (_stricmp(port, "SWO-Manchester") == 0) {
                ctx->swoPortMode = 2;
            } else if (_stricmp(port, "Off") == 0 || _stricmp(port, "None") == 0 || port[0] == '\0') {
                ctx->swoPortMode = 0;   // Trace=Off：用户没开 trace
            } else {
                // 未知端口名：按 UART 处理并留一条日志，避免"选了却不生效"这种哑失败
                ctx->swoPortMode = 1;
                LOG_WARN("CMSIS_DAP_ConfigureDebugger: 未知的 Trace=%s，按 SWO-UART 处理", port);
            }

            // 同步给流式通路：它会用这个端口决定 DAP_SWO_Mode 的参数，
            // 以及在 GetSinkDetails 里回报 sink 类型（SWO-UART / SWO-Manchester）。
            if (ctx->swoPortMode != 0) {
                ORBMDK::StreamingTrace_SetMode(static_cast<uint8_t>(ctx->swoPortMode));
            }
        }

        // --- TraceTransport= ：Read（AGDI 轮询取数）还是 Stream（流式 sink 取数） ---
        char transport[64] = {};
        if (GetConfigValue(config, "TraceTransport=", transport, sizeof(transport))) {
            if (_stricmp(transport, "Stream") == 0) {
                ctx->swoTransportMode = 2;
            } else if (_stricmp(transport, "Read") == 0) {
                ctx->swoTransportMode = 1;
            } else {
                ctx->swoTransportMode = 0;  // None / 其它写法：当作没开
            }
        }

        // --- TraceBaudrate= ：SWO 波特率 ---
        char baudStr[32] = {};
        if (GetConfigValue(config, "TraceBaudrate=", baudStr, sizeof(baudStr))) {
            const unsigned long baud = strtoul(baudStr, nullptr, 10);
            if (baud > 0) {
                ctx->swoBaudrate = static_cast<int>(baud);
                ORBMDK::StreamingTrace_SetBaudrate(static_cast<uint32_t>(baud));
            }
        }

        LOG_INFO("CMSIS_DAP_ConfigureDebugger: Trace port=%d (0=Off/1=UART/2=Manchester), "
                 "transport=%d (0=None/1=Read/2=Stream), baud=%d",
                 ctx->swoPortMode, ctx->swoTransportMode, ctx->swoBaudrate);
    }
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：CMSIS_DAP_Atomic_Control / CMSIS_DAP_Atomic_Result
//
// 官方 CMSIS_DAP.dll 反汇编（RVA 0x14D80 / 0x14DA0）：
//   Atomic_Control  只读 [ebp+8]、[ebp+0Ch]                 -> 2 个参数
//   Atomic_Result   读到 [ebp+1Ch]                          -> 6 个参数
// 两者都把参数原样转发给内部对象，函数体内不直接解引用指针。
//
// 旧版本给 Atomic_Control 声明了 5 个参数（含 uint8_t* request / int* response），
// 按官方 2 参调用时会读到栈上的垃圾指针并写入 -> 崩溃。
// 由于 ARM/Keil 头文件均未定义这两个函数的语义，这里只保留正确的参数个数，
// 不触碰任何参数指针。
// ---------------------------------------------------------------------------
RDDI_FUNC int CMSIS_DAP_Atomic_Control(const RDDIHandle handle, const int reserved)
{
    LOG_DEBUG("CMSIS_DAP_Atomic_Control: enter, handle=%d, reserved=%d", handle, reserved);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    (void)reserved;
    return RDDI_DAP_LEVEL1_NOT_IMPL;
}

RDDI_FUNC int CMSIS_DAP_Atomic_Result(const RDDIHandle handle, const int a2, const int a3,
                                      const int a4, const int a5, const int a6)
{
    LOG_DEBUG("CMSIS_DAP_Atomic_Result: enter, handle=%d, a2=%d, a3=%d", handle, a2, a3);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return RDDI_DAP_LEVEL1_NOT_IMPL;
}

// ============================================================================
// DAP 高级序列函数实现
// ============================================================================

// ARM rddi_dap.h:
//   int DAP_RegReadBlock(handle, DAP_ID, numRegs, const int *regIDArray, int *dataArray)
// 数组中每个元素是独立的 regID（bit16 = RnW），而非"同一个 regID 重复 N 次"。
RDDI_FUNC int DAP_RegReadBlock(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                               const int *regIDArray, int *dataArray)
{
    LOG_DEBUG("DAP_RegReadBlock: enter, handle=%d, numRegs=%d", handle, numRegs);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!regIDArray || !dataArray || numRegs <= 0) {
        return RDDI_BADARG;
    }

    for (int i = 0; i < numRegs; i++) {
        int status = DAP_ReadReg(handle, DAP_ID, regIDArray[i], &dataArray[i]);
        if (status != RDDI_SUCCESS) {
            LOG_ERROR("DAP_RegReadBlock[%d]: regID=0x%08X status=%d", i, regIDArray[i], status);
            return RDDI_DAP_MULTI_REG_CMD_ERR;
        }
    }

    return RDDI_SUCCESS;
}

// ARM rddi_dap.h:
//   int DAP_RegWriteBlock(handle, DAP_ID, numRegs, const int *regIDArray, const int *dataArray)
RDDI_FUNC int DAP_RegWriteBlock(const RDDIHandle handle, const int DAP_ID, const int numRegs,
                                const int *regIDArray, const int *dataArray)
{
    LOG_DEBUG("DAP_RegWriteBlock: enter, handle=%d, numRegs=%d", handle, numRegs);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!regIDArray || !dataArray || numRegs <= 0) {
        return RDDI_BADARG;
    }

    for (int i = 0; i < numRegs; i++) {
        int status = DAP_WriteReg(handle, DAP_ID, regIDArray[i], dataArray[i]);
        if (status != RDDI_SUCCESS) {
            LOG_ERROR("DAP_RegWriteBlock[%d]: regID=0x%08X status=%d", i, regIDArray[i], status);
            return RDDI_DAP_MULTI_REG_CMD_ERR;
        }
    }

    return RDDI_SUCCESS;
}

// ARM rddi_dap.h:
//   int DAP_RegReadWaitForValue(handle, DAP_ID, numRepeats, regID,
//                               const int *mask, const int *requiredValue)
// 语义：最多读 numRepeats 次，命中 (value & *mask) == *requiredValue 立即返回
//       RDDI_SUCCESS；重试耗尽返回 RDDI_DAP_NO_MATCH(0x1013)；出错立即返回。
RDDI_FUNC int DAP_RegReadWaitForValue(const RDDIHandle handle, const int DAP_ID, const int numRepeats,
                                      const int regID, const int *mask, const int *requiredValue)
{
    LOG_DEBUG("DAP_RegReadWaitForValue: enter, handle=%d, dapId=%d, numRepeats=%d, regID=0x%08X",
              handle, DAP_ID, numRepeats, regID);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!mask || !requiredValue) {
        return RDDI_BADARG;
    }

    int repeats = (numRepeats > 0) ? numRepeats : 1;

    for (int i = 0; i < repeats; i++) {
        int value = 0;
        int status = DAP_ReadReg(handle, DAP_ID, regID, &value);
        if (status != RDDI_SUCCESS) {
            return status;
        }
        if ((value & *mask) == *requiredValue) {
            return RDDI_SUCCESS;
        }
    }

    return RDDI_DAP_NO_MATCH;
}

// ARM rddi_dap.h: int DAP_DefineSequence(handle, const int seqID, void *seqDef)
// Level 1 可选功能，本实现未定义任何自定义序列。
RDDI_FUNC int DAP_DefineSequence(const RDDIHandle handle, const int seqID, void *seqDef)
{
    LOG_DEBUG("DAP_DefineSequence: enter, handle=%d, seqID=%d", handle, seqID);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    (void)seqID;
    (void)seqDef;

    return RDDI_DAP_LEVEL1_NOT_IMPL;
}

// ARM rddi_dap.h: int DAP_RunSequence(handle, const int seqID, void *seqInData, void *seqOutData)
RDDI_FUNC int DAP_RunSequence(const RDDIHandle handle, const int seqID,
                              void *seqInData, void *seqOutData)
{
    LOG_DEBUG("DAP_RunSequence: enter, handle=%d, seqID=%d", handle, seqID);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    (void)seqID;
    (void)seqInData;
    (void)seqOutData;

    return RDDI_DAP_LEVEL1_NOT_IMPL;
}

RDDI_FUNC int DAP_HostStatus(const RDDIHandle handle, int hostStatus, int state)
{
    LOG_DEBUG("DAP_HostStatus: enter, handle=%d, hostStatus=%d, state=%d", handle, hostStatus, state);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // hostStatus: 0=halted, 1=running
    // state: 0=off, 1=on
    int status = ORBMDK::DAP_HostStatus(static_cast<uint8_t>(hostStatus), static_cast<uint8_t>(state));
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int DAP_GetSupportedOptimisationLevel(const RDDIHandle handle, int *level)
{
    LOG_DEBUG("DAP_GetSupportedOptimisationLevel: enter, handle=%d", handle);
    if (level) {
        *level = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!level) {
        return RDDI_BADARG;
    }

    *level = ORBMDK::DAP_GetSupportedOptimisationLevel();
    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_SetCommTimeout(const RDDIHandle handle, int timeoutMs)
{
    LOG_DEBUG("DAP_SetCommTimeout: enter, handle=%d, timeoutMs=%d", handle, timeoutMs);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    ctx->timeoutMs = timeoutMs;
    return ORBMDK::DAP_SetCommTimeout(timeoutMs) == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

// ---------------------------------------------------------------------------
// DAP_Target 命令协议（ARM rddi_dap.h）
//
//   "signal_avail"          -> "sys_reset;sys_power"
//   "<signal>.on|off|read"  -> "<signal>.<state>"
//   多条命令以 ';' 分隔，响应同样以 ';' 分隔
//   状态取值：on / off / unknown / err
//
// ORBTrace 通过 CMSIS-DAP DAP_SWJ_Pins 控制 nRESET。
//   CMSIS-DAP 的引脚位编号（与 include/ORBMDK_DAP.h 的 ORBMDK_DAP_Pin 一致）：
//     bit5 = nTRST，bit7 = nRESET；1 = 释放(高)，0 = 拉低(断言)。
//   ★ 旧值 0x20 是 bit5(nTRST)：复位时下发的其实不是 nRESET（见 Todo.md）。
// sys_power 只能监测、无法驱动。
// ---------------------------------------------------------------------------
#define ORBMDK_SWJ_PIN_nRESET  0x80u

static std::string TargetCommand(const std::string &item)
{
    if (item == "signal_avail") {
        return "sys_reset;sys_power";
    }

    size_t dot = item.rfind('.');
    if (dot == std::string::npos) {
        return item + ".unknown";
    }

    const std::string signal = item.substr(0, dot);
    const std::string op     = item.substr(dot + 1);

    if (signal == "sys_reset") {
        int  pinIn   = 0;
        bool driveOk = true;

        if (op == "on") {
            driveOk = (ORBMDK::DAP_SWJ_Pins(ORBMDK_SWJ_PIN_nRESET, 0x00, &pinIn, 0) == 0);
        } else if (op == "off") {
            driveOk = (ORBMDK::DAP_SWJ_Pins(ORBMDK_SWJ_PIN_nRESET, ORBMDK_SWJ_PIN_nRESET,
                                            &pinIn, 0) == 0);
        } else if (op == "read") {
            driveOk = (ORBMDK::DAP_SWJ_Pins(0x00, 0x00, &pinIn, 0) == 0);
        } else {
            return signal + ".unknown";
        }

        if (!driveOk) {
            LOG_WARN("DAP_Target: '%s' 下发失败（DAP_SWJ_Pins 返回错误），nRESET 未动作",
                     item.c_str());
            return signal + ".err";
        }
        LOG_DEBUG("DAP_Target: '%s' ok, pins=0x%02X", item.c_str(), pinIn);
        // 驱动后也回读：nRESET 为低表示已断言
        bool asserted = (op == "on") || ((pinIn & ORBMDK_SWJ_PIN_nRESET) == 0);
        return signal + (asserted ? ".on" : ".off");
    }

    if (signal == "sys_power") {
        if (op == "read") {
            return signal + (ORBMDK::ORBMDK_HID_IsConnected() ? ".on" : ".off");
        }
        if (op == "on" || op == "off") {
            return signal + ".err";  // 无法驱动供电
        }
        return signal + ".unknown";
    }

    return signal + ".unknown";
}

// ARM rddi_dap.h:
//   int DAP_Target(handle, const char *request_str, char *resp_str, const int resp_len)
RDDI_FUNC int DAP_Target(const RDDIHandle handle, const char *request_str,
                         char *resp_str, const int resp_len)
{
    LOG_DEBUG("DAP_Target: enter, handle=%d, req='%s'", handle, request_str ? request_str : "(null)");
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // ★ 反汇编 CMSIS_AGDI.dll 的硬件复位路径（0x10022F78 / 0x10023051 / 0x1002315D）：
    //   AGDI 以 DAP_Target(h, "sys_reset.on", NULL, 0) 调用 —— **响应缓冲可以是 NULL/0**，
    //   它只看返回值：非 0 即上报 AGDI 错误码 0x2028，而该码在 AGDI 错误串分派表
    //   （0x10021BC0 + 表 0x10021DE4/0x10021F28）里正是 "RDDI-DAP Error"。
    //   官方 CMSIS_DAP.dll 后端（0x10019F00）同样只在 resp_len != 0 时才动缓冲、
    //   其余一律照常执行并返回 0。所以 NULL/0 必须解释成"不需要响应"。
    if (!request_str) {
        return RDDI_BADARG;
    }

    const bool wantResp = (resp_str != NULL && resp_len > 0);
    if (wantResp) {
        resp_str[0] = '\0';
    }

    std::vector<std::string> responses;
    std::string req(request_str);
    size_t pos = 0;

    while (pos < req.size()) {
        size_t sep = req.find(';', pos);
        std::string item = (sep == std::string::npos) ? req.substr(pos)
                                                      : req.substr(pos, sep - pos);
        pos = (sep == std::string::npos) ? req.size() : sep + 1;

        if (item.empty()) {
            continue;
        }
        responses.push_back(TargetCommand(item));
    }

    std::string joined;
    for (size_t i = 0; i < responses.size(); i++) {
        if (i) joined += ';';
        joined += responses[i];
    }

    if (wantResp) {
        if (joined.size() + 1 > static_cast<size_t>(resp_len)) {
            return RDDI_TARGET_COMMAND_RESPONSE_TOO_SMALL;
        }
        memcpy(resp_str, joined.c_str(), joined.size() + 1);
    }

    return RDDI_SUCCESS;
}

// ============================================================================
// StreamingTrace 函数实现
//
// 参数个数/顺序全部按反汇编 CMSIS_AGDI.dll 的**逐个调用点**对齐（见 ORBMDK_RDDI.h）：
//   Connect(handle)                                            [0x1003CE95]
//   GetSinkCount(handle,&n)                                    [0x1003CEB3]
//   GetSinkDetails(handle,i,Rec)  i = 0..n-1                   [0x1003CED8]
//   Attach(handle,idx) / Start(handle,idx)                     [0x1003D5B4 / 0x1003D5C4]
//   SubmitEventBuffer(handle,idx,Entry,type,&token)            [0x1003CF93]
//   WaitForEvent(handle,idx,&token,50)  0=有事件 0x204=无事件   [0x1003D661]
//   Detach(handle,idx) / Flush(handle,idx) / Stop(handle,idx)  [0x1003D5D8 / 0x1003D00D / 0x1003D01A]
// 导出槽位表（槽位地址 → 函数）：0x10362fdc Connect / fe0 Disconnect / fe4 GetSinkCount /
//   fe8 GetSinkDetails / fec GetConfigItem / ff0 SetConfigItem / ff4 Attach / ff8 Detach /
//   ffc SubmitEventBuffer / 0x10363000 WaitForEvent / 004 Start / 008 Flush / 00c Stop。
//
// 数据通路（"AGDI 给缓冲、本层填、再回 token 通知"）：
//   AGDI 依 GetSinkDetails 声明的 bufCount/bufSize 自行分配 bufCount 个条目，
//   每个条目 = { int kind(+0x00); void* buffer(+0x04); int size(+0x08); int length(+0x0C) }。
//   AGDI 逐个 SubmitEventBuffer 交给我们并拿到 token；我们在 WaitForEvent 里挑一条
//   把 SWO 数据读进 entry->buffer、写 entry->length，再把对应 token 回填给 AGDI。
//   ★ entry->size 就是缓冲容量，绝不可写超；bufSize/bufCount 由本层在 GetSinkDetails 里声明。
// ============================================================================

// AGDI 预填记录（0x20 字节；记录区基址 0x10363020，步长 0x20）。前 16 字节由 AGDI 写好，
// 后 8 字节（+0x10 / +0x14）AGDI **只读不写**（反汇编 0x1003D466/D4E9/D56E 均为读），
// 必须由本层填写，否则 AGDI 会分配 0 条 / 0 字节的事件缓冲、trace 窗口永远为空。
struct RddiSinkDetails {
    char *nameBuf;    // +0x00 AGDI 预填（0x10363120 + i*0x80）
    int   nameCap;    // +0x04 = 0x80
    char *typeBuf;    // +0x08 AGDI 预填（0x10363520 + i*0x80）
    int   typeCap;    // +0x0C = 0x80
    int   bufSize;    // +0x10 ★ 本层填：单条事件缓冲容量（字节）
    int   bufCount;   // +0x14 ★ 本层填：事件缓冲条数
    int   reserved0;
    int   reserved1;
};

// AGDI 分配的事件条目（16 字节；见 0x1003D506 填 +0x04、0x1003D572 填 +0x08）。
struct RddiSinkEntry {
    int   kind;    // +0x00 提交前置 3；AGDI 消费后清零
    void *buffer;  // +0x04 AGDI 分配的数据缓冲
    int   size;    // +0x08 缓冲容量
    int   length;  // +0x0C ★ 本层填：本次填入的有效字节数
};

static const char kTraceSinkName[]           = "cmsis_dap_swo_trace";  // AGDI 按名字面量匹配
static const char kTraceSinkTypeUart[]       = "SWO-UART";             // 端口 = UART（caps 0x04）
static const char kTraceSinkTypeManchester[] = "SWO-Manchester";       // 端口 = Manchester（caps 0x08）
static const int  kTraceSinkBufSize  = 4096;                   // 单条事件缓冲
static const int  kTraceSinkBufCount = 4;                      // 事件缓冲条数
static const int  kStreamingNoEvent  = 0x204;                  // AGDI：本轮无事件

// 当前端口对应的 sink 类型串。AGDI 拿到它之后会拼成
// `Trace=<type>;TraceTransport=Stream;` 再回传给 ConfigureDebugger，
// 因此这里必须与用户实际选中的端口一致 —— 恒报 "SWO-UART" 会把
// "用户选了 Manchester" 这件事在配置串里悄悄改写掉，前后自相矛盾。
static const char* TraceSinkTypeOf(const RDDIContext* ctx)
{
    return (ctx && ctx->swoPortMode == 2) ? kTraceSinkTypeManchester : kTraceSinkTypeUart;
}

// 探针是否具备 SWO 流式能力（streaming sink 是否该报出来）。与 CMSIS_DAP_Capabilities
// 的能力位收口**同侧**，不得一边说没能力、一边又报出 sink：
//   - probeCaps < 0（老固件不认 DAP_Info 0xF0）→ 乐观，允许报 sink；
//   - probeCaps >= 0 → 必须自报 INFO_CAPS_SWO_STREAMING_TRACE(0x40)，
//     且至少有一个 SWO 端口位（0x04/0x08），否则没有数据源可流。
static bool ProbeSupportsSwoStreaming(const RDDIContext* ctx)
{
    if (!ctx || ctx->probeCaps < 0) {
        return true;
    }
    return ((ctx->probeCaps & INFO_CAPS_SWO_STREAMING_TRACE) != 0) &&
           ((ctx->probeCaps & (INFO_CAPS_SWO_UART | INFO_CAPS_SWO_MANCHESTER)) != 0);
}

// 往 AGDI 预填的缓冲里做有界拷贝；指针/容量不合理时**跳过**（绝不解引用野指针）。
static void WriteBoundedString(char *dst, int cap, const char *src)
{
    if (!dst || cap <= 0 || cap > 0x10000) {
        return;
    }
    const size_t n = strlen(src);
    const size_t m = (n < static_cast<size_t>(cap - 1)) ? n : static_cast<size_t>(cap - 1);
    memcpy(dst, src, m);
    dst[m] = '\0';
}

RDDI_FUNC int StreamingTrace_Connect(const RDDIHandle handle)
{
    LOG_DEBUG("StreamingTrace_Connect: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    ORBMDK::StreamingTrace_Init();
    ctx->traceConnected = true;
    LOG_INFO("StreamingTrace_Connect: sink connected (handle=%d)", handle);
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Disconnect(const RDDIHandle handle)
{
    LOG_DEBUG("StreamingTrace_Disconnect: enter, handle=%d", handle);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    ctx->traceConnected = false;
    ctx->traceRunning   = false;
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Attach(const RDDIHandle handle, int sinkIndex)
{
    LOG_DEBUG("StreamingTrace_Attach: enter, handle=%d, sinkIndex=%d", handle, sinkIndex);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0) {
        return RDDI_BADARG;
    }

    std::lock_guard<std::mutex> lock(ctx->traceMutex);
    if (ctx->traceAttachRefCount++ == 0) {
        // 首次 attach：清条目表、建会话
        ctx->traceSinkIndex  = sinkIndex;
        ctx->traceEntryCount = 0;
        ctx->traceNextToken  = 1;
        for (int i = 0; i < RDDIContext::kTraceMaxEntries; ++i) {
            ctx->traceEntryToken[i]  = 0;
            ctx->traceEntryPtr[i]    = nullptr;
            ctx->traceEntryFilled[i] = false;
        }
        ctx->traceEntryCapacityLogged = 0;
        ORBMDK::StreamingTrace_Init();
        ctx->traceAttached = true;
    }
    LOG_INFO("StreamingTrace_Attach: sink=%d refCount=%d", sinkIndex, ctx->traceAttachRefCount);
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Detach(const RDDIHandle handle, int sinkIndex)
{
    LOG_DEBUG("StreamingTrace_Detach: enter, handle=%d, sinkIndex=%d", handle, sinkIndex);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0) {
        return RDDI_BADARG;
    }

    std::lock_guard<std::mutex> lock(ctx->traceMutex);
    if (ctx->traceAttachRefCount > 0 && --ctx->traceAttachRefCount == 0) {
        ORBMDK::StreamingTrace_Shutdown();
        ctx->traceAttached   = false;
        ctx->traceRunning    = false;
        ctx->traceSinkIndex  = -1;
        ctx->traceEntryCount = 0;
        for (int i = 0; i < RDDIContext::kTraceMaxEntries; ++i) {
            ctx->traceEntryToken[i]  = 0;
            ctx->traceEntryPtr[i]    = nullptr;
            ctx->traceEntryFilled[i] = false;
        }
    }
    LOG_INFO("StreamingTrace_Detach: sink=%d refCount=%d", sinkIndex, ctx->traceAttachRefCount);
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Start(const RDDIHandle handle, int sinkIndex)
{
    LOG_DEBUG("StreamingTrace_Start: enter, handle=%d, sinkIndex=%d", handle, sinkIndex);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0) {
        return RDDI_BADARG;
    }

    // sink 0 对应 SWO（端口是 UART 还是 Manchester 由用户的 Trace= 选择决定）。
    // 上电序列 Transport → Mode → Baudrate → Control 在 HID 层完成，
    // 其中 Mode 取自 StreamingTrace_SetMode 设好的端口，故这里先同步一次：
    // ConfigureDebugger 通常已经设过，但 AGDI 也有先 Start 后配置的调用顺序，
    // 端口必须在使用之前就位（默认 1 = UART，与旧行为一致）。
    ORBMDK::StreamingTrace_SetMode(static_cast<uint8_t>(ctx->swoPortMode == 2 ? 2 : 1));

    const int status = ORBMDK::StreamingTrace_Start(1 /* SWO */);
    if (status == 0) {
        ctx->traceMode    = 1;
        ctx->traceRunning = true;
        LOG_INFO("StreamingTrace_Start: sink=%d started (port=%s)", sinkIndex,
                 ctx->swoPortMode == 2 ? "SWO-Manchester" : "SWO-UART");
        return RDDI_SUCCESS;
    }
    LOG_WARN("StreamingTrace_Start: sink=%d failed (status=%d)", sinkIndex, status);
    return RDDI_FAILED;
}

RDDI_FUNC int StreamingTrace_Stop(const RDDIHandle handle, int sinkIndex)
{
    LOG_DEBUG("StreamingTrace_Stop: enter, handle=%d, sinkIndex=%d", handle, sinkIndex);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0) {
        return RDDI_BADARG;
    }
    const int status = ORBMDK::StreamingTrace_Stop();
    ctx->traceRunning = false;
    return (status == 0) ? RDDI_SUCCESS : RDDI_FAILED;
}

RDDI_FUNC int StreamingTrace_Flush(const RDDIHandle handle, int sinkIndex)
{
    LOG_DEBUG("StreamingTrace_Flush: enter, handle=%d, sinkIndex=%d", handle, sinkIndex);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0) {
        return RDDI_BADARG;
    }
    const int status = ORBMDK::StreamingTrace_Flush();
    return (status == 0) ? RDDI_SUCCESS : RDDI_FAILED;
}

// 返回：0 = 有事件（*evToken 回填 token）；0x204 = 本轮无事件；其他非零 = 出错。
RDDI_FUNC int StreamingTrace_WaitForEvent(const RDDIHandle handle, int sinkIndex,
                                          int *evToken, int timeoutMs)
{
    LOG_DEBUG("StreamingTrace_WaitForEvent: enter, handle=%d, sinkIndex=%d, timeoutMs=%d",
              handle, sinkIndex, timeoutMs);
    if (evToken) {
        *evToken = 0;
    }
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0 || !evToken) {
        return RDDI_BADARG;
    }

    const DWORD deadline = GetTickCount() + static_cast<DWORD>((timeoutMs > 0) ? timeoutMs : 0);

    for (;;) {
        // 取一条 AGDI 已提交、尚未回填的条目
        void *entryPtr = nullptr;
        int   slot     = -1;
        int   tok      = 0;
        {
            std::lock_guard<std::mutex> lock(ctx->traceMutex);
            for (int i = 0; i < RDDIContext::kTraceMaxEntries; ++i) {
                if (ctx->traceEntryFilled[i] && ctx->traceEntryPtr[i]) {
                    entryPtr = ctx->traceEntryPtr[i];
                    tok      = ctx->traceEntryToken[i];
                    slot     = i;
                    break;
                }
            }
        }

        if (slot < 0) {
            // 还没有任何已提交条目：无事件可报，回 0x204 让 AGDI 继续循环。
            return kStreamingNoEvent;
        }

        RddiSinkEntry* e = static_cast<RddiSinkEntry*>(entryPtr);
        const int cap = e->size;
        if (!e->buffer || cap <= 0) {
            LOG_WARN("StreamingTrace_WaitForEvent: 条目 %p 容量非法 (buf=%p cap=%d)，丢弃",
                     entryPtr, e->buffer, cap);
            std::lock_guard<std::mutex> lock(ctx->traceMutex);
            ctx->traceEntryFilled[slot] = false;
            ctx->traceEntryPtr[slot]    = nullptr;
            ctx->traceEntryToken[slot]  = 0;
            continue;
        }

        const DWORD now = GetTickCount();
        if (timeoutMs <= 0 || now >= deadline) {
            return kStreamingNoEvent;
        }

        size_t got = static_cast<size_t>(cap);
        const int rc = ORBMDK::StreamingTrace_Read(reinterpret_cast<uint8_t*>(e->buffer),
                                                   &got, deadline - now);
        if (rc > 0 && got > 0) {
            e->length = static_cast<int>(got);
            std::lock_guard<std::mutex> lock(ctx->traceMutex);
            ctx->traceEntryFilled[slot] = false;  // AGDI 消费后会重提交
            ctx->traceEntryPtr[slot]    = nullptr;
            ctx->traceEntryToken[slot]  = 0;
            *evToken = tok;
            return RDDI_SUCCESS;
        }
        // rc == 0（超时）或 rc < 0（未运行）：都按"无事件"处理，让 AGDI 继续循环。
        return kStreamingNoEvent;
    }
}

RDDI_FUNC int StreamingTrace_SubmitEventBuffer(const RDDIHandle handle, int sinkIndex,
                                               void *entry, int entryType, int *token)
{
    LOG_DEBUG("StreamingTrace_SubmitEventBuffer: enter, handle=%d, sinkIndex=%d, entry=%p",
              handle, sinkIndex, entry);
    if (token) {
        *token = 0;
    }
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (sinkIndex != 0 || !entry) {
        return RDDI_BADARG;
    }

    std::lock_guard<std::mutex> lock(ctx->traceMutex);

    // 同一个 entry 会被 AGDI 反复提交（消费后重提交），按指针对齐复用槽位。
    int slot     = -1;
    int freeSlot = -1;
    for (int i = 0; i < RDDIContext::kTraceMaxEntries; ++i) {
        if (ctx->traceEntryPtr[i] == entry) {
            slot = i;
            break;
        }
        if (freeSlot < 0 && ctx->traceEntryToken[i] == 0 && ctx->traceEntryPtr[i] == nullptr) {
            freeSlot = i;
        }
    }
    if (slot < 0) {
        slot = freeSlot;
    }
    if (slot < 0) {
        LOG_ERROR("StreamingTrace_SubmitEventBuffer: 条目表已满(%d)，拒绝 entry=%p",
                  RDDIContext::kTraceMaxEntries, entry);
        return RDDI_FAILED;
    }

    // token 不能为 0（0 被 AGDI 当作"无事件"）
    int t = ctx->traceNextToken++;
    if (ctx->traceNextToken <= 0) {
        ctx->traceNextToken = 1;
        if (t == 0) {
            t = ctx->traceNextToken++;
        }
    }

    ctx->traceEntryPtr[slot]    = entry;
    ctx->traceEntryToken[slot]  = t;
    ctx->traceEntryFilled[slot] = true;
    if (slot + 1 > ctx->traceEntryCount) {
        ctx->traceEntryCount = slot + 1;
    }
    if (!ctx->traceEntryCapacityLogged) {
        ctx->traceEntryCapacityLogged = 1;
        const RddiSinkEntry* e = static_cast<const RddiSinkEntry*>(entry);
        LOG_INFO("StreamingTrace_SubmitEventBuffer: 首个条目 entry=%p kind=%d size=%d type=%d",
                 entry, e->kind, e->size, entryType);
    }
    if (token) {
        *token = t;
    }
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_GetSinkCount(const RDDIHandle handle, int *count)
{
    LOG_DEBUG("StreamingTrace_GetSinkCount: enter, handle=%d, count=%p", handle, (void *)count);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (!count) {
        return RDDI_BADARG;
    }

    // 本层只报 1 个 sink（0 号 = cmsis_dap_swo_trace）。ETM 无对应数据源
    // （§18.10-B/§18.9）：谎报 sink 数只会让 GetSinkDetails/Attach 越界。
    //
    // ★ 但报得出 sink 的前提是探针真的能流：AGDI 拿到 count>=1 就会注册 sink 并
    //   走 Stream 传输取数。探针自报没有 SWO 流式能力时如实报 0 —— 报 0 只是让
    //   AGDI 不注册 sink（Stream 路径自然关闭），不影响 Read 路径（那条走
    //   CMSIS_DAP_SWO_Data 轮询，与 sink 无关）。
    if (!ProbeSupportsSwoStreaming(ctx)) {
        *count = 0;
        LOG_INFO("StreamingTrace_GetSinkCount: 探针 0x%04X 无 SWO 流式能力 -> 报 0 个 sink",
                 static_cast<unsigned>(ctx->probeCaps));
        return RDDI_SUCCESS;
    }

    *count = 1;
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：StreamingTrace_GetSinkDetails
//
// AGDI 调用为 3 参 (handle, index, details)，details 指向 0x10363020 + index*0x20
// 的记录，其中前 16 字节由 AGDI **预填**为两个 {缓冲指针, 容量 0x80} 对
// （记录循环见 0x1003CECA..0x1003CEEF，预填见 AGDI 的 0x10363120/0x10363520 区）。
// 本层必须把 name/type 字符串写进去，并填写 +0x10/+0x14 的 bufSize/bufCount。
// 旧版本按 6 参声明、按 2 参实现，两个方向都错：按官方 3 参调用时会把标量当指针
// 写入 -> 崩溃；声明成 2 参又拿不到 details。现按官方 3 参对齐。
// ---------------------------------------------------------------------------
RDDI_FUNC int StreamingTrace_GetSinkDetails(const RDDIHandle handle, int index, void *details)
{
    LOG_DEBUG("StreamingTrace_GetSinkDetails: enter, handle=%d, index=%d, details=%p",
              handle, index, details);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }
    if (index != 0 || !details) {
        return RDDI_BADARG;
    }

    // 与 GetSinkCount 同侧收口：没有流式能力时不存在任何 sink，
    // 即便 AGDI 硬来问 0 号也按"索引非法"拒绝，绝不填出一条不存在的 sink。
    if (!ProbeSupportsSwoStreaming(ctx)) {
        LOG_WARN("StreamingTrace_GetSinkDetails[%d]: 探针 0x%04X 无 SWO 流式能力 -> 拒绝",
                 index, static_cast<unsigned>(ctx->probeCaps));
        return RDDI_BADARG;
    }

    RddiSinkDetails* d = static_cast<RddiSinkDetails*>(details);

    // 前 16 字节是 AGDI 预填的两个 {指针, 容量} 对，本层只负责写内容。
    WriteBoundedString(d->nameBuf, d->nameCap, kTraceSinkName);
    WriteBoundedString(d->typeBuf, d->typeCap, TraceSinkTypeOf(ctx));

    // ★ +0x10/+0x14 必须由本层填写：AGDI 的取数循环只读不写。
    d->bufSize  = kTraceSinkBufSize;
    d->bufCount = kTraceSinkBufCount;

    LOG_INFO("StreamingTrace_GetSinkDetails[%d]: name=\"%s\" type=\"%s\" bufSize=%d bufCount=%d",
             index, kTraceSinkName, TraceSinkTypeOf(ctx), d->bufSize, d->bufCount);
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_GetConfigItem(const RDDIHandle handle, int item, int *value)
{
    LOG_DEBUG("StreamingTrace_GetConfigItem: enter, handle=%d, item=%d", handle, item);
    if (value) {
        *value = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!value) {
        return RDDI_BADARG;
    }

    // 必须与 SetConfigItem 写入的是同一组变量：这里是查询（读），
    // SetConfigItem 是设置（写）。旧实现 case 0/1 返回硬编码常量，
    // 于是"设 3M 再读回 115200"，读写不对称。
    switch (item) {
        case 0:  // 波特率
            *value = ctx->swoBaudrate;
            break;
        case 1:  // 缓冲大小
            *value = ctx->traceBufferSize;
            break;
        case 2:  // 模式
            *value = ctx->traceMode;
            break;
        default:
            *value = 0;
            return RDDI_BADARG;
    }
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_SetConfigItem(const RDDIHandle handle, int item, int value)
{
    LOG_DEBUG("StreamingTrace_SetConfigItem: enter, handle=%d, item=%d, value=%d",
              handle, item, value);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    switch (item) {
        case 0:  // 波特率
            ctx->swoBaudrate = value;
            ORBMDK::StreamingTrace_SetBaudrate(static_cast<uint32_t>(value));
            break;
        case 1:  // 缓冲大小
            ctx->traceBufferSize = value;
            break;
        case 2:  // 模式
            ctx->traceMode = value;
            break;
        default:
            return RDDI_BADARG;
    }
    return RDDI_SUCCESS;
}

// ============================================================================
// RDDI 日志回调（宿主日志通道）
// ============================================================================

// 宿主回调的当前注册值。历史上 RDDI_SetLogCallback 只把回调存进 ctx、
// **从未被调用**（§8.2(4)：Keil 的日志窗口什么都收不到）。
// 现在把它接到统一日志的 CallbackSink 上（§8.3(6)）。
static RDDILogCallback g_hostLogPfn      = nullptr;
static void*           g_hostLogContext  = nullptr;
static int             g_hostLogMaxLevel = RDDI_LOGLEVEL_TRACE;

// 内部级别 → RDDI 级别（ARM rddi.h：数值越小越严重）
static int _toRddiLevel(int internalLevel)
{
    switch (internalLevel) {
        case ORBMDK_LOG_ERROR: return RDDI_LOGLEVEL_ERROR;    // 1
        case ORBMDK_LOG_WARN:  return RDDI_LOGLEVEL_WARNING;  // 2
        case ORBMDK_LOG_INFO:  return RDDI_LOGLEVEL_INFO;     // 3
        default:               return RDDI_LOGLEVEL_DEBUG;    // 4
    }
}

// CallbackSink：把一条统一日志转成宿主回调（锁外调用，见 ORBMDK_LogWrite）
static void HostLogBridge(void* /*context*/, const char* msg, int level)
{
    RDDILogCallback pfn = g_hostLogPfn;
    if (!pfn || !msg) {
        return;
    }

    const int rddiLevel = _toRddiLevel(level);

    // maxLogLevel 语义：只上报"不比自己更啰嗦"的消息（RDDI 数值 ≤ maxLogLevel）。
    // 例如 maxLogLevel = INFO(3) 时，DEBUG(4)/TRACE(5) 级别的消息不回传。
    if (g_hostLogMaxLevel >= RDDI_LOGLEVEL_FATAL && rddiLevel > g_hostLogMaxLevel) {
        return;
    }

    // 注意形参顺序：ARM 是 (context, msg, logLevel)，不可写反
    pfn(g_hostLogContext, msg, rddiLevel);
}

// ARM rddi.h: void RDDI_SetLogCallback(RDDIHandle, RDDILogCallback, void *context, int maxLogLevel)
// 注意返回值为 void，且回调形参顺序是 (context, msg, logLevel)。
RDDI_FUNC void RDDI_SetLogCallback(const RDDIHandle handle, RDDILogCallback pfn,
                                   void *context, int maxLogLevel)
{
    LOG_DEBUG("RDDI_SetLogCallback: enter, handle=%d, pfn=%p, maxLogLevel=%d",
              handle, (void *)pfn, maxLogLevel);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return;  // 返回 void，无法上报错误
    }

    ctx->logCallback         = pfn;
    ctx->logCallbackContext  = context;
    ctx->logCallbackMaxLevel = maxLogLevel;

    // 接通统一日志 → 宿主（AGDI/Keil 的日志窗口）。回调的生命周期由宿主保证：
    // 本层在 RDDI_Close 时**不**清除它（AGDI 一次会话里会反复 Open/Close，
    // 清除会让日志通道在中途断掉）；DllMain(DETACH) 时统一清空。
    g_hostLogPfn      = pfn;
    g_hostLogContext  = context;
    g_hostLogMaxLevel = maxLogLevel;
    ORBMDK_LogSetCallback(pfn ? HostLogBridge : nullptr, nullptr);

    if (pfn) {
        // 这条消息本身就会经上面的通道回到宿主，正好验证链路是否接通
        LOG_INFO("RDDI_SetLogCallback: 宿主日志通道已接通 (maxLogLevel=%d)", maxLogLevel);
    }
}

// ============================================================================
// PC_* 跟踪捕获接口实现 (Program Counter Sampling)
// ============================================================================

// PC 通道信息结构
struct PCChannelInfo {
    uint32_t type;        // 通道类型: 0=ETM, 1=ITM
    uint32_t capacity;    // 缓冲区容量
    uint32_t width;       // PC 值宽度 (bits)
};

// CMSIS-DAP V2 PC Sampling 命令 (参考 CMSIS-DAP 规范)
#define DAPV2_PC_SAMPLING_CAPTURE   0x09
#define DAPV2_PC_SAMPLING_ENABLE    0x01
#define DAPV2_PC_SAMPLING_DISABLE   0x00
#define DAPV2_PC_SAMPLING_GET_DATA  0x0A

// DWT PC Sample 通过 ITM HW 包发送 (srcAddr = 2)
// ITM HW 包格式: 0bxxxxxx10 + 数据
// PC Sample 数据: 4 字节 PC 值 (小端) 或 1 字节 0x00 (睡眠状态)

/**
 * @brief 初始化 ITM 解码器
 */
static void _initITMDecoder(RDDIContext* ctx)
{
    if (!ctx->itmDecoder) {
        ctx->itmDecoder = ORBMDK_ITM_Create();
        if (ctx->itmDecoder) {
            ORBMDK_ITM_Init(ctx->itmDecoder);
            // 强制同步
            ORBMDK_ITM_ForceSync(ctx->itmDecoder, true);
        }
    }
}

/**
 * @brief 从 trace 数据中提取 PC 采样
 * 
 * PC 采样通过 ITM HW 包发送，源地址为 2 (DWT PC Sample)
 * 数据格式：
 * - 0x00: CPU 处于睡眠状态
 * - 1-4 字节: PC 采样值 (小端序)
 */
static int _fetchPCSamplesFromTrace(RDDIContext* ctx, int maxSamples)
{
    if (!ctx || !ctx->itmDecoder) {
        return 0;
    }

    // 从 SWO 缓冲区获取数据
    // 实际数据应该通过 StreamingTrace 或 SWO_Data 获取
    // 这里处理已缓存的 trace 数据
    for (size_t i = 0; i < ctx->swoBuffer.size() && (int)ctx->pcSamples.size() < maxSamples; i++) {
        uint8_t byte = ctx->swoBuffer[i];
        ORBMDK_ITM_Pump(ctx->itmDecoder, byte);
        
        // 检查是否是 PC Sample 包
        struct ORBMDK_ITM_Packet pkt;
        if (ORBMDK_ITM_GetPacket(ctx->itmDecoder, &pkt)) {
            if (pkt.type == ITM_PT_HW && pkt.srcAddr == 2) {
                // DWT PC Sample
                if (pkt.len == 1 && pkt.d[0] == 0x00) {
                    // 睡眠状态 - 不记录 PC 值，但可以记录睡眠事件
                    // ctx->pcSleepEvents.push_back(timestamp);
                } else {
                    // PC 采样值 (小端序)
                    uint32_t pcValue = 0;
                    for (int j = 0; j < pkt.len && j < 4; j++) {
                        pcValue |= ((uint32_t)pkt.d[j]) << (j * 8);
                    }
                    ctx->pcSamples.push_back(pcValue);
                }
            }
        }
    }
    
    // 清空已处理的缓冲区
    ctx->swoBuffer.clear();
    
    return (int)ctx->pcSamples.size();
}

/**
 * @brief 处理来自 USB 的 trace 数据
 * 
 * 将原始 trace 数据送入 ITM 解码器
 */
static void _processTraceData(RDDIContext* ctx, const uint8_t* data, size_t len)
{
    if (!ctx || !data || len == 0) return;
    
    _initITMDecoder(ctx);
    
    for (size_t i = 0; i < len; i++) {
        ORBMDK_ITM_Pump(ctx->itmDecoder, data[i]);
        
        // 检查 PC Sample
        struct ORBMDK_ITM_Packet pkt;
        if (ORBMDK_ITM_GetPacket(ctx->itmDecoder, &pkt)) {
            if (pkt.type == ITM_PT_HW && pkt.srcAddr == 2) {
                // DWT PC Sample
                if (pkt.len == 1 && pkt.d[0] == 0x00) {
                    // 睡眠状态
                    ctx->pcSamples.push_back(0xFFFFFFFF);  // 特殊标记表示睡眠
                } else if (pkt.len >= 1) {
                    // PC 采样值 (小端序)
                    uint32_t pcValue = 0;
                    for (int j = 0; j < pkt.len && j < 4; j++) {
                        pcValue |= ((uint32_t)pkt.d[j]) << (j * 8);
                    }
                    ctx->pcSamples.push_back(pcValue);
                }
            }
        }
    }
}

/**
 * @brief 从 USB 端点读取 trace 数据 (V2 Bulk)
 */
static int _readTraceFromUSB(RDDIContext* ctx, int maxSamples)
{
    if (!ctx) return 0;
    
    uint8_t traceData[512];
    int totalRead = 0;
    
    // 循环读取直到缓冲区满或无数据
    while ((int)ctx->pcSamples.size() < maxSamples) {
        int bytesRead = ORBMDK_USB_Bulk_Read(traceData, sizeof(traceData), 10);
        if (bytesRead <= 0) break;
        
        _processTraceData(ctx, traceData, bytesRead);
        totalRead += bytesRead;
        
        // 如果是超时，可能没有更多数据
        if (bytesRead < (int)sizeof(traceData)) break;
    }
    
    return (int)ctx->pcSamples.size();
}

RDDI_FUNC int CMSIS_DAP_PC_Capture(const RDDIHandle handle, uint8_t control)
{
    LOG_DEBUG("CMSIS_DAP_PC_Capture: enter, handle=%d, control=0x%02X", handle, control);
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // control: bit0=1 启动采样, bit0=0 停止采样
    if (control & DAPV2_PC_SAMPLING_ENABLE) {
        ctx->pcSamplingEnabled = true;
        ctx->pcSamples.clear();
        ctx->pcTimestamps.clear();
        
        // 初始化 ITM 解码器
        _initITMDecoder(ctx);

        // 发送 DAP_PC_Sampling 命令启动采样
        uint8_t cmd[3] = {
            DAPV2_PC_SAMPLING_CAPTURE,  // 命令 ID
            0x01,                        // 启动采样
            static_cast<uint8_t>(control & 0x02 ? 0x01 : 0x00)  // 可选: 连续采样模式
        };
        uint8_t resp[4] = {0};
        size_t respLen = sizeof(resp);

        // 优先尝试 V2 Bulk
        USB_Bulk_Mode mode = ORBMDK_USB_Bulk_GetMode();
        if (mode == USB_BULK_BULK_MODE) {
            ORBMDK_USB_Bulk_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
            
            // 读取初始 trace 数据
            _readTraceFromUSB(ctx, 256);
        } else {
            // Fallback 到 HID
            ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
        }
        
        // 返回已获取的采样数
        return RDDI_SUCCESS;
    } else {
        // 停止采样
        ctx->pcSamplingEnabled = false;

        uint8_t cmd[2] = { DAPV2_PC_SAMPLING_CAPTURE, DAPV2_PC_SAMPLING_DISABLE };
        uint8_t resp[4] = {0};
        size_t respLen = sizeof(resp);

        USB_Bulk_Mode mode = ORBMDK_USB_Bulk_GetMode();
        if (mode == USB_BULK_BULK_MODE) {
            ORBMDK_USB_Bulk_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
        } else {
            ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, ORBMDK_TIMEOUT_SHORT_MS);
        }
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetNumberOfChannels(const RDDIHandle handle, int *count)
{
    LOG_DEBUG("CMSIS_DAP_PC_GetNumberOfChannels: enter, handle=%d, count=%p",
              handle, (void *)count);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    // 返回可用的 PC 采样通道数
    // ETM 有 1 个通道 (ETM 触发)
    // ITM 有 8 个通道 (ITM 激励端口 0-7)
    *count = ctx->pcChannelCount;
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetChannelInfos(const RDDIHandle handle, int *count, void *infos)
{
    LOG_DEBUG("CMSIS_DAP_PC_GetChannelInfos: enter, handle=%d, count=%p, infos=%p",
              handle, (void *)count, infos);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    if (!infos) {
        // 返回需要的结构数量
        *count = ctx->pcChannelCount;
        return RDDI_SUCCESS;
    }

    PCChannelInfo* channelInfos = static_cast<PCChannelInfo*>(infos);
    for (int i = 0; i < ctx->pcChannelCount && i < *count; i++) {
        if (i == 0) {
            // 通道 0: ETM PC Sampling
            channelInfos[i].type = 0;      // ETM
            channelInfos[i].capacity = 4096;
            channelInfos[i].width = 32;    // 32-bit Cortex-M
        } else {
            // 通道 1-7: ITM 端口 (预留)
            channelInfos[i].type = 1;      // ITM
            channelInfos[i].capacity = 1024;
            channelInfos[i].width = 32;
        }
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetCommonFrequency(const RDDIHandle handle, uint32_t *frequency)
{
    LOG_DEBUG("CMSIS_DAP_PC_GetCommonFrequency: enter, handle=%d, frequency=%p",
              handle, (void *)frequency);
    if (frequency) {
        *frequency = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!frequency) {
        return RDDI_BADARG;
    }

    // 返回 PC 采样的通用频率
    // 默认使用跟踪时钟频率
    if (ctx->pcSampleFrequency == 0) {
        // 默认 10 MHz（典型值，ETB/ITM 时钟）
        *frequency = 10000000;
    } else {
        *frequency = ctx->pcSampleFrequency;
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetData(const RDDIHandle handle, int *count, uint8_t *data)
{
    LOG_DEBUG("CMSIS_DAP_PC_GetData: enter, handle=%d, count=%p, data=%p",
              handle, (void *)count, (void *)data);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    if (!ctx->pcSamplingEnabled) {
        *count = 0;
        return RDDI_SUCCESS;
    }

    // 从 USB 端点获取新的 trace 数据并提取 PC 采样
    USB_Bulk_Mode mode = ORBMDK_USB_Bulk_GetMode();
    if (mode == USB_BULK_BULK_MODE) {
        _readTraceFromUSB(ctx, 4096);
    }

    // 返回 PC 采样数据（每个 PC 值 4 字节）
    int maxCount = *count / 4;  // count 是字节数
    int copyCount = std::min(maxCount, (int)ctx->pcSamples.size());

    if (data && copyCount > 0) {
        memcpy(data, ctx->pcSamples.data(), copyCount * sizeof(uint32_t));
    }

    // 清空已读取的数据
    ctx->pcSamples.erase(ctx->pcSamples.begin(), ctx->pcSamples.begin() + copyCount);
    *count = copyCount * 4;  // 返回实际字节数

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetValues(const RDDIHandle handle, int *count, uint32_t *values)
{
    LOG_DEBUG("CMSIS_DAP_PC_GetValues: enter, handle=%d, count=%p, values=%p",
              handle, (void *)count, (void *)values);
    if (count) {
        *count = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!count) {
        return RDDI_BADARG;
    }

    if (!ctx->pcSamplingEnabled) {
        *count = 0;
        return RDDI_SUCCESS;
    }

    // 从 USB 端点获取新的 trace 数据并提取 PC 采样
    USB_Bulk_Mode mode = ORBMDK_USB_Bulk_GetMode();
    if (mode == USB_BULK_BULK_MODE) {
        _readTraceFromUSB(ctx, *count);
    }

    // 获取 PC 采样值（32-bit 地址）
    int copyCount = std::min(*count, (int)ctx->pcSamples.size());
    if (values && copyCount > 0) {
        memcpy(values, ctx->pcSamples.data(), copyCount * sizeof(uint32_t));
    }

    // 清空已读取的数据
    ctx->pcSamples.erase(ctx->pcSamples.begin(), ctx->pcSamples.begin() + copyCount);
    *count = copyCount;

    return RDDI_SUCCESS;
}
