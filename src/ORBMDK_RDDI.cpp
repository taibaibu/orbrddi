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

using namespace ORBMDK;

#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>

// ============================================================================
// 日志模块 - 使用 OutputDebugString + DebugView
// ============================================================================

// 日志级别
enum LogLevel {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO  = 1,
    LOG_LEVEL_WARN  = 2,
    LOG_LEVEL_ERROR = 3,
};

// 日志输出开关
static bool g_logEnabled = true;

// 日志级别名称
static const char* g_logLevelNames[] = { "DEBUG", "INFO", "WARN", "ERROR" };

// 当前日志级别阈值
//
// 默认 ERROR：本 DLL 由 Keil/AGDI 加载，INFO/DEBUG 级日志（每次寄存器读写、
// 每次连接步骤）在生产使用中只会产生噪声，默认全部过滤。
//
// 排障时打开详细日志，**不需要开命令行、也不需要重启 µVision**：
//
//     在 %TEMP% 下建一个纯文本文件 ORBMDK_LOG_LEVEL，内容只写一个数字：
//
//         0 = DEBUG   1 = INFO   2 = WARN   3 = ERROR
//
//     运行中的 µVision 最多 1 秒后自动生效；**删掉该文件即恢复默认(ERROR)**，
//     同样不用重启。级别 0 时单次调试可达数百行，用完记得删。
//
// 环境变量 ORBMDK_LOG_LEVEL 仍然支持（文件名优先），但它只在进程启动时
// 读一次，改了要重启 µVision —— 所以推荐用上面的文件方式。
static int ORBMDK_LogLevel(void)
{
    static int cachedLevel = LOG_LEVEL_ERROR;
    static ULONGLONG lastCheck = 0;

    // 每条日志都查文件太贵，1 秒最多查一次；因此改动最多延迟 1 秒生效。
    const ULONGLONG now = GetTickCount64();
    if (now - lastCheck < 1000) {
        return cachedLevel;
    }
    lastCheck = now;

    // 1) %TEMP%\ORBMDK_LOG_LEVEL（可热更新，优先）
    char tmp[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tmp) > 0 && tmp[0] != '\0') {
        char path[MAX_PATH] = {0};
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sORBMDK_LOG_LEVEL", tmp);

        FILE* f = nullptr;
        if (fopen_s(&f, path, "r") == 0) {
            int lvl = -1;
            const bool ok = (fscanf_s(f, "%d", &lvl) == 1) &&
                            lvl >= LOG_LEVEL_DEBUG && lvl <= LOG_LEVEL_ERROR;
            fclose(f);
            cachedLevel = ok ? lvl : LOG_LEVEL_ERROR;
            return cachedLevel;
        }
    }

    // 2) 环境变量（启动时固定）
    const char* env = getenv("ORBMDK_LOG_LEVEL");
    if (env && *env) {
        const int lvl = atoi(env);
        if (lvl >= LOG_LEVEL_DEBUG && lvl <= LOG_LEVEL_ERROR) {
            cachedLevel = lvl;
            return cachedLevel;
        }
    }

    cachedLevel = LOG_LEVEL_ERROR;  // 默认: 仅输出 ERROR
    return cachedLevel;
}

// ---------------------------------------------------------------------------
// 文件日志（临时诊断用）
//
// µVision 这类 GUI 进程的 printf / OutputDebugString 默认看不到，因此把
// INFO 及以上的日志追加写入文件。路径可用环境变量 ORBMDK_LOG_FILE 覆盖，
// 未设置时写到 %TEMP%\ORBMDK_RDDI.log。
//
// 每次写入后立即关闭文件：否则进程会一直持有句柄，诊断时外部无法读取
// （实测 UV4.exe 会独占该文件）。
// 只记录 INFO 及以上，避免每次寄存器访问都落盘导致文件爆炸。
// ---------------------------------------------------------------------------
static void ORBMDK_LogWriteFile(const char* line)
{
    char path[MAX_PATH] = {0};
    const char* env = getenv("ORBMDK_LOG_FILE");
    if (env && *env) {
        strncpy_s(path, sizeof(path), env, _TRUNCATE);
    } else {
        char tmp[MAX_PATH] = {0};
        DWORD n = GetTempPathA(MAX_PATH, tmp);
        if (n == 0 || n >= MAX_PATH) {
            return;
        }
        _snprintf_s(path, sizeof(path), _TRUNCATE, "%sORBMDK_RDDI.log", tmp);
    }

    // AGDI 是多线程的（对话框线程 + 调试线程），并发 fputs 会让行与行交错，
    // 因此这里串行化写入。句柄仍按行开关（UV4 会独占文件，外部诊断需要能读）。
    static std::mutex fileMutex;
    std::lock_guard<std::mutex> lock(fileMutex);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0) {
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

// 内部日志函数
static void ORBMDK_Log(LogLevel level, const char* module, const char* fmt, ...) {
    // 文件与调试器输出共用同一阈值。
    // 历史问题：文件日志曾写死 `level >= LOG_LEVEL_INFO`，完全绕过 ORBMDK_LogLevel()，
    // 默认阈值明明是 ERROR，例行流程的 INFO 仍会全部落盘 —— 实测一次调试产生 230+ 行，
    // 其中 224 行是噪声（RDDI_Open / CMSIS_DAP_Detect / ConfigureInterface 各约 50 次）。
    const bool enabled = (g_logEnabled && level >= ORBMDK_LogLevel());

    if (!enabled) {
        return;  // 被过滤时不构造字符串，避免无谓开销
    }

    // 时间戳 + 进程/线程 ID：日志跨进程追加（每次 Keil 会话一个新进程），
    // 且 AGDI 多线程调用。没有这两项时，多次运行混在一起会出现
    // "RDDI_Open 返回的 handle 全是 1" 这类现象，极易误判为句柄泄漏。
    SYSTEMTIME st;
    GetLocalTime(&st);

    char buffer[1024];
    int offset = snprintf(buffer, sizeof(buffer),
                          "[ORBMDK][%02u:%02u:%02u.%03u][%s][%s][%lu:%lu] ",
                          st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                          g_logLevelNames[level], module ? module : "-",
                          (unsigned long)GetCurrentProcessId(),
                          (unsigned long)GetCurrentThreadId());
    if (offset < 0) {
        return;
    }
    if (offset >= (int)sizeof(buffer)) {
        offset = (int)sizeof(buffer) - 1;
    }

    // 添加用户消息
    if (fmt && offset < (int)sizeof(buffer) - 1) {
        va_list args;
        va_start(args, fmt);
        const int n = vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
        va_end(args);
        if (n < 0) {
            buffer[offset] = '\0';
        } else if (offset + n >= (int)sizeof(buffer) - 1) {
            // 明确标记截断：静默截断会让日志"看起来完整"却少了后半段
            const int tail = (int)sizeof(buffer) - 5;
            if (tail > offset) {
                memcpy(buffer + tail, "...\0", 4);
            }
        }
    }

    ORBMDK_LogWriteFile(buffer);

    printf("%s\n", buffer);
    OutputDebugStringA(buffer);
}

// 日志宏
#define LOG_DEBUG(fmt, ...) ORBMDK_Log(LOG_LEVEL_DEBUG, "RDDI", fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ORBMDK_Log(LOG_LEVEL_INFO,  "RDDI", fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ORBMDK_Log(LOG_LEVEL_WARN,  "RDDI", fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ORBMDK_Log(LOG_LEVEL_ERROR, "RDDI", fmt, ##__VA_ARGS__)

// 模块级日志宏 (可在其他模块使用)
#define LOG_HID_DEBUG(fmt, ...)   ORBMDK_Log(LOG_LEVEL_DEBUG, "HID", fmt, ##__VA_ARGS__)
#define LOG_HID_INFO(fmt, ...)    ORBMDK_Log(LOG_LEVEL_INFO,  "HID", fmt, ##__VA_ARGS__)
#define LOG_HID_ERROR(fmt, ...)   ORBMDK_Log(LOG_LEVEL_ERROR, "HID", fmt, ##__VA_ARGS__)
#define LOG_BULK_DEBUG(fmt, ...)  ORBMDK_Log(LOG_LEVEL_DEBUG, "BULK", fmt, ##__VA_ARGS__)
#define LOG_BULK_INFO(fmt, ...)   ORBMDK_Log(LOG_LEVEL_INFO,  "BULK", fmt, ##__VA_ARGS__)
#define LOG_BULK_ERROR(fmt, ...)  ORBMDK_Log(LOG_LEVEL_ERROR, "BULK", fmt, ##__VA_ARGS__)
#define LOG_TRACE_DEBUG(fmt, ...) ORBMDK_Log(LOG_LEVEL_DEBUG, "TRACE", fmt, ##__VA_ARGS__)
#define LOG_TRACE_INFO(fmt, ...) ORBMDK_Log(LOG_LEVEL_INFO,  "TRACE", fmt, ##__VA_ARGS__)

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

// 块传输总开关，**默认开启**，实际是否使用由运行时自动探测决定。
//
// 背景：DAP_RegWriteRepeat / DAP_RegReadRepeat 原先逐字调用 DAP_Transfer，
// 每字一次 USB 往返。改用 ID_DAP_TRANSFER_BLOCK 后单次往返可带
// kMaxBlockWords(=14) 个字，约 14×（见本文第九节）。
//
// 但并非所有 CMSIS-DAP 固件都实现了 ID_DAP_TRANSFER_BLOCK —— 例如
// orbtrace 固件就没有。因此本层做**能力自动探测 + 不兼容回退**：
//   - 每个上下文首次用到块传输时，先用一次只读探测（读 DP IDCODE）确认；
//   - 不支持则永久回退到逐字 DAP_Transfer，功能不受影响，只是慢；
//   - 传输过程中块传输若意外失败，也会立即回退（双保险）。
//
// 排障时可用环境变量强制关闭（跳过探测，直接走逐字传输）：
//     set ORBMDK_BLOCK_TRANSFER=0
static bool BlockTransferEnabled()
{
    static const bool enabled = []() -> bool {
        const char* env = getenv("ORBMDK_BLOCK_TRANSFER");
        if (env && *env) {
            return (*env != '0');   // 显式设置时才看它
        }
        return true;                // 默认开启，由自动探测决定实际路径
    }();
    return enabled;
}

// ============================================================================
// Context - 使用 map 管理，避免 vector 扩容导致指针失效
// ============================================================================

struct RDDIContext {
    bool initialized = false;
    int handle = -1;  // Handle 索引

    // 块传输可用性。默认乐观开启，首次用到时由 EnsureBlockTransferProbed()
    // 探测一次并落定；探测或后续传输失败都会置 false，之后永久回退到
    // 逐字 DAP_Transfer（见 BlockTransferEnabled 的说明）。
    bool blockTransferSupported = true;
    bool blockTransferProbed    = false;

    // Debug configuration
    bool isSWD = true;
    uint32_t debugClock = kDefaultClock;
    bool isConnected = false;

    // Device info
    uint32_t capabilities = INFO_CAPS_SWD | INFO_CAPS_ATOMIC_CMDS;
    std::string productName;
    std::string serialNumber;
    std::string firmwareVersion;

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
// 探测失败只影响性能，不影响功能 —— 之后永久走逐字 DAP_Transfer。
// ---------------------------------------------------------------------------
static bool ProbeBlockTransfer(int dapId)
{
    uint32_t idcode = 0;
    const int status = ORBMDK::DAP_TransferBlock(dapId, 1, 0x02 /* read IDCODE */,
                                                 nullptr, &idcode);
    if (status == ORBMDK::DAP_RES_OK) {
        LOG_INFO("Block transfer probe OK (ID_DAP_TRANSFER_BLOCK supported, idcode=0x%08X)",
                 idcode);
        return true;
    }
    LOG_WARN("Block transfer probe failed (status=%d) -> firmware does not support "
             "ID_DAP_TRANSFER_BLOCK, falling back to single transfers", status);
    return false;
}

// 首次用到块传输时调用；之后再调用无开销。
static void EnsureBlockTransferProbed(RDDIContext* ctx, int dapId)
{
    if (ctx->blockTransferProbed) {
        return;
    }
    ctx->blockTransferProbed = true;

    if (!BlockTransferEnabled()) {
        ctx->blockTransferSupported = false;
        LOG_INFO("Block transfer disabled by ORBMDK_BLOCK_TRANSFER -> single transfers");
        return;
    }

    ctx->blockTransferSupported = ProbeBlockTransfer(dapId);
}

// ============================================================================
// RDDI Core Functions
// ============================================================================

RDDI_FUNC int RDDI_Open(RDDIHandle *pHandle, const void *pDetails)
{
    LOG_DEBUG("RDDI_Open called, pDetails=%p", pDetails);

    if (!pHandle) {
        return RDDI_BADARG;
    }

    std::lock_guard<std::mutex> lock(gContextMutex);

    // 打开设备：优先 CMSIS-DAP V2（USB Bulk），失败自动回退 V1（HID）。
    // ORBMDK_USB_Bulk_Init 内部已实现该优先级与回退逻辑。
    //
    // 注意**不要**把这件事放进 ORBMDK_HID_OpenDevice：它会持有 g_hidMutex，
    // 而回退路径又会调用 ORBMDK_HID_OpenDevice，std::mutex 非递归会死锁。
    if (ORBMDK_USB_Bulk_GetMode() == USB_BULK_NOT_INITED &&
        !ORBMDK::ORBMDK_HID_IsConnected()) {
        LOG_DEBUG("RDDI_Open: no transport yet, trying V2 Bulk then V1 HID...");
        if (ORBMDK_USB_Bulk_Init(0, 0, nullptr) != 0) {
            // 带上系统错误码：否则现场只能看到"失败了"，无从判断是
            // 设备未插、被独占、还是驱动问题。
            const DWORD sysErr = GetLastError();
            LOG_ERROR("RDDI_Open: open failed (V2 Bulk and V1 HID both failed), "
                      "GetLastError()=%lu", (unsigned long)sysErr);
            // 失败时必须把句柄置 0（本层句柄从 1 开始，0 恒为无效值）：
            // AGDI 的 `rddi_Open(&handle, NULL)` 之后直接用该变量当句柄，
            // 不写的话它会保留上一次成功打开时的旧句柄，继续对着一个
            // 已失效的连接调用各接口。
            *pHandle = 0;
            return RDDI_FAILED;
        }
        LOG_INFO("RDDI_Open: transport = %s",
                 ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE
                     ? "CMSIS-DAP v2 (USB Bulk)"
                     : "CMSIS-DAP v1 (HID)");
    }

    // 分配新句柄
    RDDIHandle handle = gNextHandle++;

    // 创建新的上下文（unique_ptr 保证生命周期）
    auto ctx = std::make_unique<RDDIContext>();
    ctx->initialized = true;
    ctx->handle = handle;
    ctx->isConnected = true;
    ctx->blockTransferSupported = BlockTransferEnabled();  // 默认关闭，见其说明

    // 从 HID 层取设备标识（USB 产品名 / 序列号 / 固件版本）。
    // 历史问题：这三个字段此前从未被赋值，导致 CMSIS_DAP_Identify 只能回退到
    // 硬编码串、CMSIS_DAP_GetDeviceIDList 返回空字符串、CMSIS_DAP_GetGUID 得到
    // "ORBTrace-"。数据其实早已由 HidD_GetProductString / HidD_GetSerialNumberString
    // 取到，只是没有向上传递。
    {
        char product[128] = {};
        char serial[128] = {};
        char version[64] = {};
        if (ORBMDK::ORBMDK_HID_GetDeviceInfo(product, sizeof(product),
                                             serial, sizeof(serial),
                                             version, sizeof(version)) == 0) {
            ctx->productName     = product;
            ctx->serialNumber    = serial;
            ctx->firmwareVersion = version;
            LOG_DEBUG("RDDI_Open: device product='%s' serial='%s' fw='%s'",
                      ctx->productName.c_str(), ctx->serialNumber.c_str(),
                      ctx->firmwareVersion.c_str());
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
                LOG_INFO("RDDI_Open: V2 product name = '%s'", ctx->productName.c_str());
            } else {
                LOG_WARN("RDDI_Open: V2 interface string unavailable, keeping '%s'",
                         ctx->productName.c_str());
            }
        }
    }

    // 插入 map（堆分配，地址稳定）
    gContexts.emplace(handle, std::move(ctx));

    *pHandle = handle;

    LOG_INFO("RDDI_Open: OK, handle=%d", handle);
    return RDDI_SUCCESS;
}

RDDI_FUNC int RDDI_Close(RDDIHandle handle)
{
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
    LOG_DEBUG("DAP_Connect: handle=%d, pConnDetails=%p", handle, pConnDetails);

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
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!DAP_ID_Array || sizeOfArray < sizeof(int)) {
        return RDDI_BADARG;
    }

    // 与 DAP_GetNumberOfDAPs 保持一致：连接目标之前即可返回，单 DAP，ID = 0。
    // 注意 DAP_ID 会被后续 DAP_ReadReg / DAP_WriteReg 当作 CMSIS-DAP 的
    // "DAP Index" 字节使用，必须是索引而不是 IDCODE。
    DAP_ID_Array[0] = kSingleDapId;

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_Disconnect(const RDDIHandle handle)
{
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

RDDI_FUNC int DAP_ReadReg(const RDDIHandle handle, const int dapId, const int regId, int *value)
{
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
    int status = ORBMDK::DAP_Transfer(dapId, request, &data);

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
    LOG_DEBUG("DAP_ReadReg: dapId=%d, regId=0x%08X, request=0x%02X -> 0x%08X",
              dapId, regId, request, data);
    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_WriteReg(const RDDIHandle handle, const int dapId, const int regId, const int value)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("DAP_WriteReg: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    const int id = GetRegId(regId);

    // DP ABORT：ARM 中既可写编号 0（DP 地址 0x00 的写口），也可用专用编号 8
    if ((id == 0 || id == 8) && (regId & DAP_REG_RnW) == 0) {
        LOG_DEBUG("DAP_WriteReg: ABORT (id=%d), value=0x%08X", id, value);
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
    int status = ORBMDK::DAP_Transfer(dapId, request, &data);

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

    // 状态 LED：跟踪 AP TAR(5) / DRW(7) 写入（官方 AGDI 不调用 DAP_HostStatus）
    if (id == 5) {          // DAP_REG_AP_0x4 = TAR
        TrackApTarWrite(ctx, static_cast<uint32_t>(value));
    } else if (id == 7) {   // DAP_REG_AP_0xC = DRW
        TrackApDataWrite(ctx, static_cast<uint32_t>(value));
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_RegAccessBlock(const RDDIHandle handle, const int dapId, const int numRegs,
                                 const int *regIdArray, int *dataArray)
{
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

    // Build transfer request sequence
    for (int i = 0; i < numRegs; i++) {
        const int regId = regIdArray[i];
        const int id    = GetRegId(regId);

        if (id == 16) {              // MATCH_MASK
            matchMask    = dataArray[i];
            matchMaskSet = true;
            continue;
        }
        if (id == 17) {              // MATCH_RETRY
            matchRetry = dataArray[i];
            continue;
        }

        const int offset = GetRegOffset(regId);
        if (offset < 0) {
            LOG_ERROR("DAP_RegAccessBlock[%d]: unsupported regID=0x%08X", i, regId);
            return RDDI_DAP_BAD_REGISTER_ID;
        }

        const bool isRead = (regId & DAP_REG_RnW) != 0;
        const bool isWait = (regId & DAP_REG_WaitForValue) != 0;

        uint32_t data = 0;
        int status = 0;

        if (isRead) {
            const uint8_t request = static_cast<uint8_t>(offset) | 0x02;
            if (isWait) {
                // 读并等待值匹配：dataArray[i] 是期望值，掩码取自 MATCH_MASK。
                // 本次调用未给出 MATCH_MASK 时按期望值中为 1 的位匹配
                // （"等这些位置起来"），而不是全等比较 —— 见 matchMaskSet 的说明。
                const int expected      = dataArray[i];
                const int effectiveMask = matchMaskSet ? matchMask : expected;
                const int retries       = (matchRetry > 0) ? matchRetry : 1;
                status = ORBMDK::DAP_RES_ERROR;
                for (int r = 0; r < retries; r++) {
                    data = 0;
                    status = ORBMDK::DAP_Transfer(dapId, request, &data);
                    if (status != ORBMDK::DAP_RES_OK) {
                        break;
                    }
                    if ((static_cast<int>(data) & effectiveMask) == (expected & effectiveMask)) {
                        break;
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
            } else {
                status = ORBMDK::DAP_Transfer(dapId, request, &data);
            }
            if (status == ORBMDK::DAP_RES_OK) {
                dataArray[i] = static_cast<int>(data);
            }
        } else {
            const uint8_t request = static_cast<uint8_t>(offset);
            data = static_cast<uint32_t>(dataArray[i]);
            status = ORBMDK::DAP_Transfer(dapId, request, &data);
            if (status == ORBMDK::DAP_RES_OK) {
                // 状态 LED：跟踪 AP TAR(5) / DRW(7) 写入。
                // AGDI 通过 SWD_WriteData（TAR + DRW）写 DHCSR，即走本分支。
                if (id == 5) {          // TAR
                    TrackApTarWrite(ctx, data);
                } else if (id == 7) {   // DRW
                    TrackApDataWrite(ctx, data);
                }
            }
        }

        if (status != ORBMDK::DAP_RES_OK) {
            if (status == ORBMDK::DAP_RES_FAULT) {
                return RDDI_DAP_DP_STICKY_ERR;
            }
            if (status == ORBMDK::DAP_RES_WAIT || status == ORBMDK::DAP_RES_NO_ACK) {
                return RDDI_DAP_OPERATION_TIMEOUT;
            }
            return RDDI_INTERNAL_ERROR;
        }
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
// 改为块传输（ID_DAP_TRANSFER_BLOCK）后，单次往返从 1 个字提升到
// kMaxBlockWords(=14) 个字，约 14×。AGDI 侧已把 CSW 配成地址自增
// （SWD_WriteBlock / SWD_VerifyBlock），语义与块传输一致。
//
// 固件若不支持该命令，首次失败即永久回退到逐字传输，功能不受影响。
// ---------------------------------------------------------------------------
RDDI_FUNC int DAP_RegWriteRepeat(const RDDIHandle handle, const int dapId, const int numRepeats,
                                 const int regId, const int *dataArray)
{
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

    for (int done = 0; done < numRepeats; ) {
        int chunk = numRepeats - done;
        if (chunk > kMaxBlockWords) {
            chunk = kMaxBlockWords;
        }

        if (ctx->blockTransferSupported) {
            const int status = ORBMDK::DAP_TransferBlock(
                dapId, static_cast<uint16_t>(chunk), request,
                reinterpret_cast<const uint32_t*>(dataArray + done), nullptr);
            if (status == ORBMDK::DAP_RES_OK) {
                done += chunk;
                continue;
            }
            ctx->blockTransferSupported = false;
            LOG_WARN("DAP_RegWriteRepeat: block transfer failed (status=%d), "
                     "falling back to single transfers", status);
        }

        for (int i = 0; i < chunk; i++) {
            uint32_t data = static_cast<uint32_t>(dataArray[done + i]);
            const int status = ORBMDK::DAP_Transfer(dapId, request, &data);
            if (status != ORBMDK::DAP_RES_OK) {
                LOG_ERROR("DAP_RegWriteRepeat: single write failed at %d/%d, status=%d",
                          done + i, numRepeats, status);
                return RDDI_DAP_ERROR;
            }
        }
        done += chunk;
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int DAP_RegReadRepeat(const RDDIHandle handle, const int dapId, const int numRepeats,
                                const int regId, int *dataArray)
{
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

    for (int done = 0; done < numRepeats; ) {
        int chunk = numRepeats - done;
        if (chunk > kMaxBlockWords) {
            chunk = kMaxBlockWords;
        }

        if (ctx->blockTransferSupported) {
            // 先清零本块：块传输若只回部分数据，剩余元素不会残留上一次的值
            for (int i = 0; i < chunk; i++) {
                dataArray[done + i] = 0;
            }
            const int status = ORBMDK::DAP_TransferBlock(
                dapId, static_cast<uint16_t>(chunk), request,
                nullptr, reinterpret_cast<uint32_t*>(dataArray + done));
            if (status == ORBMDK::DAP_RES_OK) {
                done += chunk;
                continue;
            }
            ctx->blockTransferSupported = false;
            LOG_WARN("DAP_RegReadRepeat: block transfer failed (status=%d), "
                     "falling back to single transfers", status);
        }

        for (int i = 0; i < chunk; i++) {
            uint32_t data = 0;
            const int status = ORBMDK::DAP_Transfer(dapId, request, &data);
            if (status != ORBMDK::DAP_RES_OK) {
                LOG_ERROR("DAP_RegReadRepeat: single read failed at %d/%d, status=%d",
                          done + i, numRepeats, status);
                return RDDI_DAP_ERROR;
            }
            dataArray[done + i] = static_cast<int>(data);
        }
        done += chunk;
    }

    return RDDI_SUCCESS;
}

// ============================================================================
// CMSIS-DAP Specific Functions
// ============================================================================

RDDI_FUNC int CMSIS_DAP_Detect(const RDDIHandle handle, int *noOfIFs)
{
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

    *noOfIFs = 1;  // One interface (ORBTrace 单设备)
    LOG_INFO("CMSIS_DAP_Detect: noOfIFs=1");
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_Identify(const RDDIHandle handle, int ifNo, int idNo,
                                   char *str, const int len)
{
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

    // 优先使用 RDDI_Open 时从 HID 层取到的设备标识；
    // 取不到再向固件查询 DAP_Info；最后才用硬编码兜底。
    switch (idNo) {
        case 1:  // Vendor (RDDI_CMSIS_DAP_ID_VENDOR)
            ORBMDK::DAP_GetInfo(DAP_INFO_VENDOR, str, len);
            if (str[0] == '\0') {
                strncpy_s(str, len, "ORBTrace", len - 1);
            }
            break;

        case 2:  // Product (RDDI_CMSIS_DAP_ID_PRODUCT)
            if (!ctx->productName.empty()) {
                strncpy_s(str, len, ctx->productName.c_str(), len - 1);
            } else {
                ORBMDK::DAP_GetInfo(DAP_INFO_PRODUCT, str, len);
                if (str[0] == '\0') {
                    strncpy_s(str, len, "ORBTrace CMSIS-DAP", len - 1);
                }
            }
            break;

        case 3:  // Serial number (RDDI_CMSIS_DAP_ID_SER_NUM)
            if (!ctx->serialNumber.empty()) {
                strncpy_s(str, len, ctx->serialNumber.c_str(), len - 1);
            } else {
                ORBMDK::DAP_GetInfo(DAP_INFO_SERIAL, str, len);
                if (str[0] == '\0') {
                    strncpy_s(str, len, "Unknown", len - 1);
                }
            }
            break;

        case 4:  // Firmware version (RDDI_CMSIS_DAP_ID_FW_VER)
            if (!ctx->firmwareVersion.empty()) {
                strncpy_s(str, len, ctx->firmwareVersion.c_str(), len - 1);
            } else {
                ORBMDK::DAP_GetInfo(DAP_INFO_FIRMWARE, str, len);
                if (str[0] == '\0') {
                    strncpy_s(str, len, "1.0.0", len - 1);
                }
            }
            break;

        default:
            // 5 = Device Vendor / 6 = Device Name：由上层 AGDI 从 PDSC 提供，
            // 本层无法回答，保持空串（已在上方清空）。
            break;
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_ConfigureInterface(const RDDIHandle handle, int ifNo, char *str)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_INFO("CMSIS_DAP_ConfigureInterface: invalid handle=%d", handle);
        return RDDI_INVHANDLE;
    }

    LOG_INFO("CMSIS_DAP_ConfigureInterface: ifNo=%d, cfg='%s'", ifNo, str ? str : "(null)");

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
            ctx->isSWD = (valueStr == "SW");
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
    *cap_info = INFO_CAPS_SWD | INFO_CAPS_ATOMIC_CMDS;

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

    // Connect to target (0=默认/自动, 1=SWD, 2=JTAG)
    int mode = ORBMDK::DAP_ConnectTarget();
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
    int status = ORBMDK::DAP_Transfer(0, 0x02, &idcode);  // Read IDCODE request

    // DPIDR 不可能为 0 或 0xFFFFFFFF。读回这些值说明目标还没进入 SWD 模式，
    // 补一次线复位后重试一次。
    if (status != ORBMDK::DAP_RES_OK || idcode == 0 || idcode == 0xFFFFFFFFu) {
        if (mode == 1) {
            uint8_t lineReset[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
            uint8_t idle[] = { 0x00 };
            ORBMDK::DAP_SWJ_Sequence(56, lineReset);
            ORBMDK::DAP_SWJ_Sequence(8, idle);
        }
        status = ORBMDK::DAP_Transfer(0, 0x02, &idcode);
    }

    ctx->dapIdList.clear();
    if (status == ORBMDK::DAP_RES_OK && idcode != 0 && idcode != 0xFFFFFFFFu) {
        ctx->dapIdList.push_back(idcode);
        if (outIdcode) {
            *outIdcode = idcode;
        }
    }

    // Connect LED：能走到这里说明 DAP_ConnectTarget 已成功，点亮它。
    // （官方 AGDI 不调用 DAP_HostStatus，必须由本层驱动，见 SetHostLed 说明）
    SetHostLed(ctx, kHostLedConnect, true);

    LOG_INFO("DetectTargetDapIdList: mode=%d transferStatus=%d idcode=0x%08X -> count=%d",
             mode, status, idcode, (int)ctx->dapIdList.size());
    return static_cast<int>(ctx->dapIdList.size());
}

RDDI_FUNC int CMSIS_DAP_DetectNumberOfDAPs(const RDDIHandle handle, int *numDAPs)
{
    LOG_INFO("CMSIS_DAP_DetectNumberOfDAPs: enter, handle=%d", handle);

    if (numDAPs) {
        *numDAPs = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_INFO("CMSIS_DAP_DetectNumberOfDAPs: invalid handle -> RDDI_INVHANDLE");
        return RDDI_INVHANDLE;
    }

    if (!numDAPs) {
        LOG_INFO("CMSIS_DAP_DetectNumberOfDAPs: null out param -> RDDI_BADARG");
        return RDDI_BADARG;
    }

    *numDAPs = DetectTargetDapIdList(ctx, nullptr, nullptr);

    LOG_INFO("CMSIS_DAP_DetectNumberOfDAPs: noOfDAPs=%d", *numDAPs);
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
    LOG_INFO("CMSIS_DAP_DetectDAPIDList: enter, handle=%d, array=%p, requested=%llu",
             handle, (void *)DAP_ID_Array, (unsigned long long)sizeOfArray);

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_INFO("CMSIS_DAP_DetectDAPIDList: invalid handle -> RDDI_INVHANDLE");
        return RDDI_INVHANDLE;
    }

    if (!DAP_ID_Array || sizeOfArray == 0) {
        LOG_INFO("CMSIS_DAP_DetectDAPIDList: BADARG (array=%p, requested=%llu)",
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

    LOG_INFO("CMSIS_DAP_DetectDAPIDList: requested=%llu, available=%llu, returned=%llu, id[0]=0x%08X",
             (unsigned long long)sizeOfArray, (unsigned long long)ctx->dapIdList.size(),
             (unsigned long long)count, (unsigned)ctx->dapIdList[0]);
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_Commands(const RDDIHandle handle, int num,
                                   unsigned char **request, int *req_len,
                                   unsigned char **response, int *resp_len)
{
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
    int status = ORBMDK::DAP_ResetTarget();
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWJ_Sequence(const RDDIHandle handle, int num,
                                       unsigned char *request)
{
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
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 发送 DAP_Delay 命令 (0x09)
    uint8_t cmd[3] = { 0x09, (uint8_t)(delay_us & 0xFF), (uint8_t)((delay_us >> 8) & 0xFF) };
    uint8_t resp[2] = {0};
    size_t respLen = sizeof(resp);
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
    return ret == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

RDDI_FUNC int CMSIS_DAP_ResetTarget(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_ResetTarget();
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_Connect(const RDDIHandle handle, int *connectedInterface)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_Connect: invalid handle");
        return RDDI_INVHANDLE;
    }

    LOG_DEBUG("CMSIS_DAP_Connect: starting SWD connection sequence");

    // 连接目标并获取模式 (0=默认/自动, 1=SWD, 2=JTAG)
    int mode = ORBMDK::DAP_ConnectTarget();
    LOG_DEBUG("CMSIS_DAP_Connect: DAP_ConnectTarget returned mode=%d", mode);
    if (mode < 0 || mode > 2) {
        LOG_ERROR("CMSIS_DAP_Connect: DAP_ConnectTarget failed, mode=%d", mode);
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

    // 返回连接的接口类型
    if (connectedInterface) {
        *connectedInterface = mode;  // 1=SWD, 2=JTAG
    }

    // 设置连接状态
    ctx->isConnected = true;
    LOG_DEBUG("CMSIS_DAP_Connect: connection complete, interface=%s", mode == 1 ? "SWD" : "JTAG");

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_SWJ_Clock(const RDDIHandle handle, unsigned int clock)
{
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
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_WriteAbort(dap_id, abort);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWD_Configure(const RDDIHandle handle, uint8_t cfg)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_ConfigureSWD(cfg);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_SWD_Sequence(const RDDIHandle handle, int num, unsigned char *request)
{
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
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, 2 + dataBytes, resp, &respLen, 100);
    // 原实现检查 resp[0]==1，但 resp[0] 是报告ID(0x00)，导致恒为失败。
    if (ret != 0) return RDDI_FAILED;
    if (respLen >= 3 && resp[1] == ID_DAP_SWD_SEQUENCE && resp[2] != 0) return RDDI_FAILED;
    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_JTAG_Configure(const RDDIHandle handle, int count, uint8_t *ir_len)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_JTAG_Configure(ir_len[0], (uint8_t)count);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_JTAG_Sequence(const RDDIHandle handle, int num, uint8_t *info,
                                       uint8_t *tdi, uint8_t *tdo, uint8_t mask)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::DAP_JTAG_Sequence((uint8_t)num, (uint8_t)num, tdi, tdo);
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

RDDI_FUNC int CMSIS_DAP_JTAG_GetIDCODEs(const RDDIHandle handle, int *count, uint32_t *idcodes)
{
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
    // 同 GetIDCODEs：先无条件写输出参数，避免调用方读到栈残留。
    if (count) {
        *count = 0;
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // JTAG IR 长度通常在 JTAG_Configure 中获取
    // 这里返回默认值
    (void)lengths;
    return RDDI_SUCCESS;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Control(handle, int control)
//   control: 0 = Stop, 1 = Start
RDDI_FUNC int CMSIS_DAP_SWO_Control(const RDDIHandle handle, int control)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
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
    // 响应（含报告ID）：[报告ID][命令ID][Status]
    uint8_t cmd[1] = { ID_DAP_SWO_STATUS };
    uint8_t resp[8] = {0};
    size_t respLen = sizeof(resp);
    int ret = ORBMDK::ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);

    if (ret != 0 || respLen < 3 || resp[1] != ID_DAP_SWO_STATUS) {
        return RDDI_FAILED;
    }

    *status = resp[2];
    // 本实现按需从探针拉取 SWO 数据，不做本地缓存，故待读字节数为 0。
    if (count) {
        *count = static_cast<int>(ctx->swoBuffer.size());
    }
    return RDDI_SUCCESS;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Baudrate(handle, int baudrate)
RDDI_FUNC int CMSIS_DAP_SWO_Baudrate(const RDDIHandle handle, int baudrate)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (baudrate <= 0) {
        return RDDI_BADARG;
    }

    int status = ORBMDK::DAP_SWO_Baudrate(static_cast<uint32_t>(baudrate));
    return status == 0 ? RDDI_SUCCESS : RDDI_DAP_ERROR;
}

// ARM rddi_dap_swo.h: CMSIS_DAP_SWO_Data(handle, int *num_written, void *buffer, int *status)
//   num_written: [in] buffer 容量，[out] 实际写入字节数
// 旧签名缺少 status 输出参数。
RDDI_FUNC int CMSIS_DAP_SWO_Data(const RDDIHandle handle, int *num_written,
                                 void *buffer, int *status)
{
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
    LOG_INFO("CMSIS_DAP_GetNumberOfDevices: count=%d", *count);
    return RDDI_SUCCESS;
}

// CMSIS_DAP_ResetDAP - 重置 DAP 调试器
RDDI_FUNC int CMSIS_DAP_ResetDAP(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        LOG_ERROR("CMSIS_DAP_ResetDAP: invalid handle");
        return RDDI_INVHANDLE;
    }

    LOG_DEBUG("CMSIS_DAP_ResetDAP: starting DAP reset sequence");

    // 执行 DAP 复位序列
    // 1. 重新连接目标 (0=默认/自动, 1=SWD, 2=JTAG)
    int mode = ORBMDK::DAP_ConnectTarget();
    if (mode < 0 || mode > 2) {
        LOG_ERROR("CMSIS_DAP_ResetDAP: DAP_ConnectTarget failed, mode=%d", mode);
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

    // 3. 配置传输参数
    ORBMDK::DAP_ConfigureSWD(0);
    ORBMDK::DAP_ConfigureTransfer(0, kDefaultWaitRetry, kDefaultMatchRetry);

    // 4. 重置目标
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
    LOG_INFO("CMSIS_DAP_DetectNumberOfDevices: count=%d", *count);
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
        return RDDI_BADARG;
    }

    // sizeOfArray 是字节数，可容纳的元素个数按 sizeof(int) 折算
    const int maxEntries = static_cast<int>(sizeOfArray / sizeof(int));
    if (maxEntries <= 0) {
        return RDDI_SUCCESS;
    }

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

    LOG_INFO("CMSIS_DAP_GetDeviceIDList: maxEntries=%d, returned=%d, id[0]=0x%08X",
             maxEntries, n, (unsigned)ctx->dapIdList[0]);
    return RDDI_SUCCESS;
}

// DAP_GetSupportedHostStatusIDs - 获取支持的主机状态 ID
RDDI_FUNC int DAP_GetSupportedHostStatusIDs(const RDDIHandle handle, int *count, int *statusIDs)
{
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

RDDI_FUNC int CMSIS_DAP_ConfigureDebugger(const RDDIHandle handle, const char *config)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (config) {
        // 解析调试器配置字符串
        ctx->lastErrorStr = config;
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
// ORBTrace 通过 CMSIS-DAP DAP_SWJ_Pins 控制 nRESET（bit5，1 = 释放，0 = 拉低）。
// sys_power 只能监测、无法驱动。
// ---------------------------------------------------------------------------
#define ORBMDK_SWJ_PIN_nRESET  0x20u

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
            return signal + ".err";
        }
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
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!request_str || !resp_str || resp_len <= 0) {
        return RDDI_BADARG;
    }

    resp_str[0] = '\0';

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

    if (joined.size() + 1 > static_cast<size_t>(resp_len)) {
        return RDDI_TARGET_COMMAND_RESPONSE_TOO_SMALL;
    }
    memcpy(resp_str, joined.c_str(), joined.size() + 1);

    return RDDI_SUCCESS;
}

// ============================================================================
// StreamingTrace 函数实现
// ============================================================================

RDDI_FUNC int StreamingTrace_Attach(const RDDIHandle handle, const char *sinkName)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 初始化 trace 子系统
    ORBMDK::StreamingTrace_Init();
    ctx->traceAttached = true;

    (void)sinkName;
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Detach(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (ctx->traceAttached) {
        ORBMDK::StreamingTrace_Shutdown();
        ctx->traceAttached = false;
    }
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Connect(const RDDIHandle handle, const char *sinkName, int mode)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // mode: 0=none, 1=SWO, 2=ETM
    if (mode == 1 || mode == 2) {
        ctx->traceMode = mode;
        ctx->traceConnected = true;
        return RDDI_SUCCESS;
    }

    return RDDI_BADARG;
}

RDDI_FUNC int StreamingTrace_Disconnect(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    ctx->traceConnected = false;
    ctx->traceRunning = false;
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_Start(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!ctx->traceConnected) {
        return RDDI_BADARG;
    }

    int status = ORBMDK::StreamingTrace_Start(static_cast<uint8_t>(ctx->traceMode));
    if (status == 0) {
        ctx->traceRunning = true;
    }
    return status == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

RDDI_FUNC int StreamingTrace_Stop(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    int status = ORBMDK::StreamingTrace_Stop();
    ctx->traceRunning = false;
    return status == 0 ? RDDI_SUCCESS : RDDI_FAILED;
}

RDDI_FUNC int StreamingTrace_Flush(const RDDIHandle handle)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 清空内部缓冲区
    ctx->traceBuffer.clear();
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_WaitForEvent(const RDDIHandle handle, int timeoutMs, int *eventType)
{
    if (eventType) {
        *eventType = 0;  // 失败时也要写输出参数
    }

    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    if (!eventType) {
        return RDDI_BADARG;
    }

    uint8_t status = 0;
    uint16_t traceCount = 0;

    DWORD startTime = GetTickCount();
    while (GetTickCount() - startTime < (DWORD)timeoutMs) {
        int ret = ORBMDK::StreamingTrace_GetStatus(&status, &traceCount);
        if (ret == 0 && traceCount > 0) {
            *eventType = 1;  // Data available
            return RDDI_SUCCESS;
        }
        Sleep(1);
    }

    *eventType = 0;
    return RDDI_TIMEOUT;
}

RDDI_FUNC int StreamingTrace_SubmitEventBuffer(const RDDIHandle handle, uint8_t *buffer, int bufferSize)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    // 用于事件驱动的缓冲区提交（ETM trace）
    (void)buffer;
    (void)bufferSize;
    return RDDI_SUCCESS;
}

RDDI_FUNC int StreamingTrace_GetSinkCount(const RDDIHandle handle, int *count)
{
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

    // 返回可用的 trace sink 数量
    *count = 2;  // SWO + ETM
    return RDDI_SUCCESS;
}

// ---------------------------------------------------------------------------
// Keil 扩展：StreamingTrace_GetSinkDetails
//
// 官方 CMSIS_DAP.dll 反汇编（RVA 0xBF00）只读 [ebp+8]/[ebp+0Ch]/[ebp+10h]，
// 即 3 个参数，并把后两个原样转发，函数体内不写调用方缓冲区。
//
// 旧版本声明为 6 个参数（char *name / char *type 两个输出缓冲区），按官方 3 参
// 调用时第 3、4 个参数取到的是标量，会被当成字符串缓冲区写入 -> 崩溃。
// 这里按官方参数个数对齐，并且不再向未经证实的指针写入。
// ---------------------------------------------------------------------------
RDDI_FUNC int StreamingTrace_GetSinkDetails(const RDDIHandle handle, const int reserved1,
                                            const int reserved2)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    (void)reserved1;
    (void)reserved2;
    return RDDI_DAP_LEVEL1_NOT_IMPL;
}

RDDI_FUNC int StreamingTrace_GetConfigItem(const RDDIHandle handle, int item, int *value)
{
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

    switch (item) {
        case 0:  // 波特率
            *value = 115200;
            break;
        case 1:  // 缓冲大小
            *value = 65536;
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
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return RDDI_INVHANDLE;
    }

    switch (item) {
        case 0:  // 波特率
            ORBMDK::DAP_SWO_Baudrate(static_cast<uint32_t>(value));
            ctx->swoBaudrate = value;
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
// RDDI 日志回调
// ============================================================================

// ARM rddi.h: void RDDI_SetLogCallback(RDDIHandle, RDDILogCallback, void *context, int maxLogLevel)
// 注意返回值为 void，且回调形参顺序是 (context, msg, logLevel)。
RDDI_FUNC void RDDI_SetLogCallback(const RDDIHandle handle, RDDILogCallback pfn,
                                   void *context, int maxLogLevel)
{
    RDDIContext* ctx = GetContext(handle);
    if (!ctx) {
        return;  // 返回 void，无法上报错误
    }

    ctx->logCallback         = pfn;
    ctx->logCallbackContext  = context;
    ctx->logCallbackMaxLevel = maxLogLevel;
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
            ORBMDK_USB_Bulk_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
            
            // 读取初始 trace 数据
            _readTraceFromUSB(ctx, 256);
        } else {
            // Fallback 到 HID
            ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
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
            ORBMDK_USB_Bulk_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
        } else {
            ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
        }
    }

    return RDDI_SUCCESS;
}

RDDI_FUNC int CMSIS_DAP_PC_GetNumberOfChannels(const RDDIHandle handle, int *count)
{
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
