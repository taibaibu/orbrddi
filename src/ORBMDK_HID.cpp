/**
 * @file ORBMDK_HID.cpp
 * @brief USB HID 通信层实现
 */

#include "pch.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_HID.h"
#include "ORBMDK_Log.h"

#include <hidsdi.h>
#include <setupapi.h>
#include <vector>
#include <string>
#include <thread>
#include <condition_variable>
#include <chrono>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// ============================================================================
// 日志（统一实现见 src/ORBMDK_Log.cpp，COMPAT_ANALYSIS §8.3）
//
// ★ 2026-09-30：删除了本文件自己的 HID_LogLevel / HID_Log 与那套宏 ——
//   它原来只走 stdout/DebugView、**不落盘**，级别也只认环境变量（启动固定），
//   与 RDDI 那套重复且能力不对等。现在两者完全一致：
//   落盘 + %TEMP%\ORBMDK_LOG_LEVEL 热更新 + 统一格式（时间戳/进程线程号）。
// ============================================================================
#define ORBMDK_LOG_MODULE "HID"
#define LOG_HID_DEBUG(...) ORBMDK_LOG_DEBUG(__VA_ARGS__)
#define LOG_HID_INFO(...)  ORBMDK_LOG_INFO(__VA_ARGS__)
#define LOG_HID_WARN(...)  ORBMDK_LOG_WARN(__VA_ARGS__)
#define LOG_HID_ERROR(...) ORBMDK_LOG_ERROR(__VA_ARGS__)

// ---------------------------------------------------------------------------
// 命令级日志：**不受级别控制**，总是落盘（与 ORBMDK_USB_Bulk.cpp 的 BulkTrace 同构）。
//
// 为什么必须有：V1(HID) 路径下的 DAP 命令原本**完全不可见**（默认阈值 ERROR 且
// 当时 HID 根本不落文件）。实测排障时表现为"AGDI 在 CMSIS_DAP_Connect 之后什么都
// 不做就断开"，而 HID 命令其实一直在发。传输层的排障线索不允许依赖日志级别。
//
// 迁移到统一实现后：仍然不受级别控制、仍然只落盘，但会遵循 ORBMDK_LOG_FILE，
// 并带上时间戳与进程/线程号（原来是手写的 "[ORBMDK][INFO][HID]" 前缀）。
// ---------------------------------------------------------------------------
#define HidTrace(...) ORBMDK_LOG_TRACE(__VA_ARGS__)

// ============================================================================
// Constants
// ============================================================================

// ORBTrace 设备 VID/PID
// 统一取自 ORBMDK.h，保证与 USB Bulk 层 (ORBMDK_USB_Bulk.cpp) 一致。
// 历史问题：此处曾为 0x3456，而 Bulk 层为 0x6A02，两者会匹配到不同设备。
static constexpr uint16_t ORBTRACE_VID = ORBMDK_ORBTRACE_VID;
static constexpr uint16_t ORBTRACE_PID = ORBMDK_ORBTRACE_PID;

// 标准 CMSIS-DAP 兼容设备列表 (常见调试器)
static constexpr uint16_t CMSIS_DAP_VIDS[] = {
    0x0D28,  // ARM
    0x2E03,  // ORBTrace / 用户设备
    0x1209,  // ORBTrace (备用)
    0xC251,  // Keil
    0x1366,  // Segger
    0x0483,  // ST
};

// 标准 CMSIS-DAP 使用报告ID = 0x00
static constexpr uint8_t CMD_REPORT_ID = 0x00U;
static constexpr size_t HID_MAX_PACKET_SIZE = 65;  // 64 + 1 Report ID
static constexpr size_t DAP_BUFFER_SIZE = 512;

// ============================================================================
// Globals
// ============================================================================

static HANDLE g_hDevice = INVALID_HANDLE_VALUE;
static std::mutex g_hidMutex;
static bool g_isConnected = false;
static char g_productName[256] = {0};
static char g_serialNumber[256] = {0};
static char g_firmwareVersion[64] = {0};

// ============================================================================
// Forward declarations
// ============================================================================

static int SendDAPCommand(uint8_t cmdId, const uint8_t* data, size_t dataLen);
static int DAP_Info(uint8_t infoId, char* buffer, size_t bufferLen);
static int DAP_ConnectSWD(void);
static int DAP_SWDConfigure(uint8_t config);
static int DAP_TransferConfigure(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry);
static int DAP_SWJ_Clock(uint32_t clock);

// ============================================================================
// USB HID Implementation
// ============================================================================

namespace ORBMDK {

int ORBMDK_HID_Init(void)
{
    std::lock_guard<std::mutex> lock(g_hidMutex);
    g_hDevice = INVALID_HANDLE_VALUE;
    g_isConnected = false;
    return 0;
}

void ORBMDK_HID_Shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_hidMutex);
    if (g_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hDevice);
        g_hDevice = INVALID_HANDLE_VALUE;
    }
    g_isConnected = false;
}

// 检查 VID 是否在支持列表中
static bool IsCMSISDAPDevice(uint16_t vid)
{
    for (size_t i = 0; i < sizeof(CMSIS_DAP_VIDS) / sizeof(CMSIS_DAP_VIDS[0]); i++) {
        if (vid == CMSIS_DAP_VIDS[i]) return true;
    }
    return false;
}

static bool EnumerateDevices(std::vector<std::string>& serials)
{
    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfo == INVALID_HANDLE_VALUE) return false;

    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(
            deviceInfo, nullptr, &hidGuid, i, &interfaceData); i++) {

        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetail(
            deviceInfo, &interfaceData, nullptr, 0, &detailSize, nullptr);

        if (detailSize == 0) continue;

        std::vector<uint8_t> detailBuffer(detailSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(
                deviceInfo, &interfaceData, detail, detailSize, nullptr, nullptr)) {
            continue;
        }

        HANDLE hDevice = CreateFile(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

        if (hDevice == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);

        if (HidD_GetAttributes(hDevice, &attributes)) {
            // 支持任何 CMSIS-DAP 兼容设备
            if (IsCMSISDAPDevice(attributes.VendorID)) {
                wchar_t serialBuffer[256] = {};
                if (HidD_GetSerialNumberString(hDevice, serialBuffer, sizeof(serialBuffer))) {
                    char serial[256] = {};
                    WideCharToMultiByte(CP_ACP, 0, serialBuffer, -1, serial, sizeof(serial), nullptr, nullptr);
                    serials.push_back(serial);
                }
            }
        }
        CloseHandle(hDevice);
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);
    return !serials.empty();
}

static bool ReadDeviceInfo(HANDLE hDevice)
{
    wchar_t productBuffer[256] = {};
    if (HidD_GetProductString(hDevice, productBuffer, sizeof(productBuffer))) {
        WideCharToMultiByte(CP_ACP, 0, productBuffer, -1, g_productName, sizeof(g_productName), nullptr, nullptr);
    } else {
        strcpy_s(g_productName, "ORBTrace");
    }

    wchar_t serialBuffer[256] = {};
    if (HidD_GetSerialNumberString(hDevice, serialBuffer, sizeof(serialBuffer))) {
        WideCharToMultiByte(CP_ACP, 0, serialBuffer, -1, g_serialNumber, sizeof(g_serialNumber), nullptr, nullptr);
    } else {
        strcpy_s(g_serialNumber, "Unknown");
    }

    // 固件版本**不在这里取**：HidD_GetAttributes 的 VersionNumber（= 设备描述符的
    // bcdDevice）是 USB 栈写的，与固件真实版本无关，已弃用。
    // 版本一律**问设备**（CMSIS-DAP DAP_Info / idNo=4），见 ORBMDK_HID_GetDeviceInfo。
    g_firmwareVersion[0] = '\0';
    return true;
}

int ORBMDK_HID_OpenDevice(const char* serial)
{
    std::lock_guard<std::mutex> lock(g_hidMutex);

    if (g_isConnected) return 0;

    std::vector<std::string> serials;
    if (!serial || serial[0] == '\0') {
        if (!EnumerateDevices(serials)) return -1;
        serial = serials[0].c_str();
    }

    GUID hidGuid;
    HidD_GetHidGuid(&hidGuid);

    HDEVINFO deviceInfo = SetupDiGetClassDevs(&hidGuid, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfo == INVALID_HANDLE_VALUE) return -1;

    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);

    bool found = false;
    HANDLE hDevice = INVALID_HANDLE_VALUE;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(
            deviceInfo, nullptr, &hidGuid, i, &interfaceData); i++) {

        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetail(
            deviceInfo, &interfaceData, nullptr, 0, &detailSize, nullptr);

        if (detailSize == 0) continue;

        std::vector<uint8_t> detailBuffer(detailSize);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA*>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(
                deviceInfo, &interfaceData, detail, detailSize, nullptr, nullptr)) {
            continue;
        }

        hDevice = CreateFile(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);

        if (hDevice == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attributes = {};
        attributes.Size = sizeof(attributes);

        if (HidD_GetAttributes(hDevice, &attributes) &&
            IsCMSISDAPDevice(attributes.VendorID)) {

            wchar_t serialBuffer[256] = {};
            if (HidD_GetSerialNumberString(hDevice, serialBuffer, sizeof(serialBuffer))) {
                char deviceSerial[256] = {};
                WideCharToMultiByte(CP_ACP, 0, serialBuffer, -1,
                    deviceSerial, sizeof(deviceSerial), nullptr, nullptr);

                if (strcmp(serial, deviceSerial) == 0) {
                    found = true;
                    break;
                }
            } else if (!serial || serial[0] == '\0') {
                found = true;
                break;
            }
        }

        CloseHandle(hDevice);
        hDevice = INVALID_HANDLE_VALUE;
    }

    SetupDiDestroyDeviceInfoList(deviceInfo);

    if (!found) {
        if (hDevice != INVALID_HANDLE_VALUE) CloseHandle(hDevice);
        return -1;
    }

    if (!ReadDeviceInfo(hDevice)) {
        CloseHandle(hDevice);
        return -1;
    }

    g_hDevice = hDevice;
    g_isConnected = true;

    // 设备刚打开：清掉上一次会话在这个进程里留下的熔断状态（bug.md B1）。
    // "探针已重新上电"这条恢复路径不能被熔断挡住；真的是哑机的话，随后的
    // DAP_Info 会立刻把它重新熔断（代价只有一两条命令）。
    ORBMDK_DapChannelReset("HID 设备已重新打开");
    return 0;
}

void ORBMDK_HID_CloseDevice(void)
{
    std::lock_guard<std::mutex> lock(g_hidMutex);
    if (g_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(g_hDevice);
        g_hDevice = INVALID_HANDLE_VALUE;
    }
    g_isConnected = false;
}

int ORBMDK_HID_IsConnected(void)
{
    std::lock_guard<std::mutex> lock(g_hidMutex);
    return g_isConnected ? 1 : 0;
}

int ORBMDK_HID_GetDeviceInfo(char* product, size_t productLen,
                              char* serial, size_t serialLen,
                              char* version, size_t versionLen)
{
    {
        std::lock_guard<std::mutex> lock(g_hidMutex);
        if (!g_isConnected) return -1;

        if (product) strncpy_s(product, productLen, g_productName, productLen - 1);
        if (serial) strncpy_s(serial, serialLen, g_serialNumber, serialLen - 1);
        if (version) version[0] = '\0';

        if (!version || versionLen == 0) {
            return 0;                        // 没要版本，收工
        }
        if (g_firmwareVersion[0] != '\0') {  // 有缓存就直接回（避免多余的 USB 往返）
            strncpy_s(version, versionLen, g_firmwareVersion, versionLen - 1);
            return 0;
        }
    }

    // 版本必须**问设备**（CMSIS-DAP DAP_Info / idNo=4）—— 描述符里的版本字段
    // （HidD_GetAttributes 的 VersionNumber）与固件版本无关，已弃用。
    //
    // ⚠ 这次调用**必须**在 g_hidMutex 释放之后：V1 路径的 DAP_Info 内部自己会加
    // 这把锁（std::mutex 非递归），放在上面那个 lock_guard 作用域里会自死锁。
    const int got = DAP_GetInfo(DAP_INFO_FIRMWARE, version, versionLen);
    if (got <= 0) {
        version[0] = '\0';
        LOG_HID_WARN("GetDeviceInfo: DAP_Info(0x04) failed (rc=%d)", got);
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_hidMutex);
    strncpy_s(g_firmwareVersion, sizeof(g_firmwareVersion), version, _TRUNCATE);
    return 0;
}

#include "ORBMDK_USB_Bulk.h"   // V2 Bulk 传输（本函数是两层共用的分发点）

// ============================================================================
// DAP 命令通道熔断（缓解；固件侧缺陷见 bug.md B1）
//
// 背景（本层改不了，要改的是固件 RTL）：
//   orbtrace 固件的 DAP 命令通道**没有任何超时兜底** —— 一次握手失败后内部 busy
//   标志永不清零（cmsis_dap.py:1712 `streamOut.ready = ~busy`），此后不再消费任何
//   命令；而 USB 层照常工作，设备照常枚举、能打开、能读描述符。现场表现为"每条
//   DAP 命令都等满 1s 超时才返回"，几十条连着来就把整场会话拖死，日志里还看不出
//   根因。主机侧没有任何"复位探针"的手段，唯一有效的恢复是给探针断电重插。
//
// 本层能做的只是**刹车**：把"连续超时"当作"设备已无响应"的证据，达到阈值后自行
// 熔断 —— 后续命令立即失败（不再逐条去等 1s），并打一条默认级别可见的 ERROR 提示
// 现场"给探针重新上电"。
//
// ★ 状态必须放**命名共享内存**，不能是普通 static：
//   AGDI 在一次会话里会反复卸载并重新加载本 DLL（见 ORBMDK_RDDI.cpp 的
//   _nextOpenSeq 说明），static 会被清零 —— 熔断刚建立就失效，等于没做。
//
// 熔断 / 恢复判据
//   熔断：连续 kDapTripAfter 次**超时**（任何一次成功即清零；其它失败既不计数也不
//         清零）。留一次容错，防 USB 抖动误杀。
//   恢复：① 任意一次命令成功；② 设备被重新打开（HID 打开 / WinUSB 初始化）；
//         ③ 熔断窗口到期后放行一条**探测命令**，成功即解除 —— 探针重新上电后
//         最迟一个窗口内自动恢复，不必重开 Keil。
//   窗口：kDapHalfOpenMs 起，探测继续失败就翻倍，封顶 kDapHalfOpenMaxMs（设备真死
//         时探测频率指数下降，不刷日志、也不反复吃 1s 超时）。
//
// 只统计"超时"，不统计协议层失败（比如固件回 0xFF 表示不支持该命令）：后者恰恰
// 说明设备活得好好的 —— 算进去会在"块传输不支持 → 回退逐字传输"这类正常回退路径
// 上误熔断。
// ============================================================================
namespace {

struct DapChannelHealth {
    volatile LONG tripped;      // 1 = 已熔断
    volatile LONG consecutive;  // 连续超时次数（成功即清零）
    volatile LONG probeFails;   // 熔断后探测连续失败次数（决定退避窗口）
    volatile LONG windowStart;  // 熔断时刻 / 上次放行探测的时刻（GetTickCount）
    volatile LONG lastNotice;   // 上次打"命令被拦下"日志的时刻（限流）
    volatile LONG suppressed;   // 熔断期间被拦下的命令总数（诊断）
    volatile LONG trips;        // 累计熔断次数（诊断）
    volatile LONG recoveries;   // 累计自愈次数（诊断）
};

constexpr LONG  kDapTripAfter     = 2;       // 连续 2 次超时即熔断
constexpr DWORD kDapHalfOpenMs    = 3000;    // 首次半开探测窗口
constexpr DWORD kDapHalfOpenMaxMs = 30000;   // 退避窗口封顶
constexpr DWORD kDapNoticeMinMs   = 10000;   // "命令被拦下"日志的最小间隔

// Local\ 作用域 = 当前登录会话；进程内跨 DLL 重载存活（与 _nextOpenSeq 同法）
DapChannelHealth* _dapHealthOf(void)
{
    static volatile DapChannelHealth* slot = nullptr;
    if (!slot) {
        HANDLE map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, (DWORD)sizeof(DapChannelHealth),
                                        "Local\\ORBMDK_DapHealth");
        if (map) {
            void* view = MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0,
                                      sizeof(DapChannelHealth));
            if (view) {
                slot = (volatile DapChannelHealth*)view;
            }
        }
    }
    return (DapChannelHealth*)slot;
}

DWORD _dapHalfOpenWindow(LONG probeFails)
{
    DWORD w = kDapHalfOpenMs;
    for (LONG i = 0; i < probeFails; ++i) {
        if (w >= kDapHalfOpenMaxMs) {
            break;
        }
        w *= 2;
    }
    return (w > kDapHalfOpenMaxMs) ? kDapHalfOpenMaxMs : w;
}

// 这个负返回值是否意味着"DAP 通道没响应（超时）"。
// ⚠ -2 的含义随传输层而变：V2 是 _bulkWrite/_bulkRead 超时，V1 却是 cmdLen 越界
//   （编程错误）—— 后者绝不能拿来熔断。
bool _dapIsTimeout(int rc)
{
    if (rc >= 0) {
        return false;
    }
    if (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) {
        return rc == -2;
    }
    return rc == -3 || rc == -5;   // V1：HID 写超时 / 读超时
}

}  // anonymous namespace

bool ORBMDK_DapChannelUsable(void)
{
    DapChannelHealth* h = _dapHealthOf();
    if (!h || !h->tripped) {
        return true;
    }

    const DWORD now = GetTickCount();
    if ((DWORD)(now - (DWORD)h->windowStart) < _dapHalfOpenWindow(h->probeFails)) {
        const LONG n = InterlockedIncrement(&h->suppressed);
        // 熔断期间命令可能是成千上万条，逐条打日志会把日志文件冲垮 —— 限流
        if ((DWORD)(now - (DWORD)h->lastNotice) >= kDapNoticeMinMs) {
            InterlockedExchange(&h->lastNotice, (LONG)now);
            LOG_HID_WARN("DAP 通道处于熔断状态，命令被立即拒绝（累计 %ld 条）。"
                         "请给探针断电重插 —— 见 bug.md B1", n);
        }
        return false;
    }

    // 窗口到期：放行这一条当探测用，并把窗口重新计时（避免多线程一次放行一大片）
    InterlockedExchange(&h->windowStart, (LONG)now);
    LOG_HID_INFO("DAP 通道熔断窗口到期，放行一条探测命令（第 %ld 次探测）",
                 (long)h->probeFails + 1);
    return true;
}

void ORBMDK_DapNoteResult(int rc)
{
    DapChannelHealth* h = _dapHealthOf();
    if (!h) {
        return;
    }

    if (rc == 0) {
        if (InterlockedExchange(&h->tripped, 0)) {
            InterlockedIncrement(&h->recoveries);
            LOG_HID_ERROR("DAP 通道已恢复：命令重新得到响应（熔断期间共拦下 %ld 条）。"
                          "可以继续调试 —— 若再次卡死，多半仍是 bug.md B1",
                          (long)h->suppressed);
        }
        InterlockedExchange(&h->consecutive, 0);
        InterlockedExchange(&h->probeFails, 0);
        return;
    }

    if (!_dapIsTimeout(rc)) {
        // 协议层失败：设备在响应，只是这条命令不被接受 —— 既不算超时，也不清计数
        return;
    }

    if (!h->tripped) {
        const LONG n = InterlockedIncrement(&h->consecutive);
        if (n >= kDapTripAfter) {
            InterlockedExchange(&h->tripped, 1);
            InterlockedExchange(&h->probeFails, 0);
            InterlockedExchange(&h->suppressed, 0);
            InterlockedExchange(&h->lastNotice, 0);
            InterlockedExchange(&h->windowStart, (LONG)GetTickCount());
            InterlockedIncrement(&h->trips);
            LOG_HID_ERROR("================ DAP 通道熔断 ================");
            LOG_HID_ERROR("连续 %ld 次超时（rc=%d），探针已不再响应任何 DAP 命令。", n, rc);
            //LOG_HID_ERROR("这是 orbtrace 固件的已知缺陷（bug.md B1）：一次握手失败后");
            //LOG_HID_ERROR("命令通道永久停止接收，而 USB 层仍然正常，主机侧无法复位它。");
            //LOG_HID_ERROR("本层从现在起不再等待 1s 超时，DAP 命令立即失败。");
            //LOG_HID_ERROR(">>> 唯一有效的恢复手段：给探针断电重插后重试本次调试。");
            LOG_HID_ERROR("重插后无需重开 Keil：最迟 %u ms 后自动恢复（窗口到期放行探测）。",
                          (unsigned)kDapHalfOpenMs);
            //LOG_HID_ERROR("==============================================");
        }
        return;
    }

    // 已在熔断态 —— 说明刚才是半开窗口放行的探测、又失败了：退避加倍后继续等
    InterlockedIncrement(&h->probeFails);
    InterlockedExchange(&h->windowStart, (LONG)GetTickCount());
}

int ORBMDK_DapFastFailCode(void)
{
    // 与"真超时"返回同一个码，上层无需为熔断新增判断分支
    return (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) ? -2 : -5;
}

void ORBMDK_DapChannelReset(const char* why)
{
    DapChannelHealth* h = _dapHealthOf();
    if (!h) {
        return;
    }

    const bool wasTripped = (InterlockedExchange(&h->tripped, 0) != 0);
    InterlockedExchange(&h->consecutive, 0);
    InterlockedExchange(&h->probeFails, 0);
    if (wasTripped) {
        LOG_HID_WARN("DAP 通道熔断状态已解除（%s）", why ? why : "设备重新打开");
    }
}

// 前向声明：熔断闸门必须包在真正的分发逻辑外面（原函数体在下面，已更名为 _DapCommandRaw）
static int _DapCommandRaw(const uint8_t* cmd, size_t cmdLen,
                          uint8_t* resp, size_t* respLen, int timeoutMs);

int ORBMDK_HID_DAPCommand(const uint8_t* cmd, size_t cmdLen,
                          uint8_t* resp, size_t* respLen, int timeoutMs)
{
    // 熔断闸门（bug.md B1）：设备已判定无响应时直接失败，不再把 1s 超时重复几十遍
    if (!ORBMDK_DapChannelUsable()) {
        if (respLen) {
            *respLen = 0;
        }
        return ORBMDK_DapFastFailCode();
    }

    const int rc = _DapCommandRaw(cmd, cmdLen, resp, respLen, timeoutMs);
    ORBMDK_DapNoteResult(rc);
    return rc;
}

static int _DapCommandRaw(const uint8_t* cmd, size_t cmdLen,
                          uint8_t* resp, size_t* respLen, int timeoutMs)
{
    // ----------------------------------------------------------------------
    // V2 Bulk 优先。
    //
    // CMSIS-DAP V2 走 USB Bulk：单包 512 字节、且没有 HID 中断端点的 1 ms
    // 轮询间隔，因此吞吐远高于 V1。是否处于 Bulk 模式由 ORBMDK_USB_Bulk_Init
    // 决定（在 RDDI_Open 中调用，内部优先尝试 Bulk，失败回退 HID）。
    //
    // 注意 V2 与 V1 的报文布局差一个字节：
    //     V1: [报告ID][命令ID][负载...]
    //     V2: [命令ID][负载...]           <- 没有报告ID
    // 为让上层所有解析（resp[0]=报告ID, resp[1]=命令ID, resp[2..]=负载）保持
    // 不变，这里把 V2 响应**规整成 V1 布局**：前置一个报告ID占位字节。
    // ----------------------------------------------------------------------
    if (ORBMDK_USB_Bulk_GetMode() == USB_BULK_BULK_MODE) {
        uint8_t tmp[1024] = {};
        size_t tmpLen = sizeof(tmp);

        const int r = ORBMDK_USB_Bulk_DAPCommand(cmd, cmdLen, tmp, &tmpLen, timeoutMs);
        if (r != 0) {
            LOG_HID_ERROR("V2 Bulk DAPCommand failed: result=%d", r);
            return r;
        }

        if (resp && respLen && *respLen > 1) {
            size_t n = tmpLen;
            if (n > *respLen - 1) {
                n = *respLen - 1;   // 截断，绝不越过调用方缓冲区
            }
            resp[0] = CMD_REPORT_ID;
            memcpy(&resp[1], tmp, n);
            *respLen = n + 1;
        }
        return 0;
    }

    std::lock_guard<std::mutex> lock(g_hidMutex);

    if (!g_isConnected || g_hDevice == INVALID_HANDLE_VALUE) {
        HidTrace("DAPCommand: cmd=0x%02X outLen=%zu -> NOT CONNECTED", cmd[0], cmdLen);
        return -1;
    }

    // 上限必须是**实际输出缓冲区**的大小，不是 DAP_BUFFER_SIZE(512)：
    // reportOut 只有 HID_MAX_PACKET_SIZE(65) 字节，下一行的
    // memcpy(&reportOut[1], cmd, cmdLen) 在 cmdLen ∈ [65, 511] 时会栈溢出。
    // 逐字 DAP_Transfer 最多 8 字节、碰不到；但接入块传输后
    // count > 14（5 + 14*4 = 61 字节）就会踩中。
    if (cmdLen == 0 || cmdLen > HID_MAX_PACKET_SIZE - 1) return -2;

    uint8_t reportOut[HID_MAX_PACKET_SIZE] = {};
    reportOut[0] = CMD_REPORT_ID;
    memcpy(&reportOut[1], cmd, cmdLen);

    // 使用 OVERLAPPED I/O 进行 WriteFile
    OVERLAPPED writeOl = {};
    writeOl.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    DWORD bytesWritten = 0;
    if (!WriteFile(g_hDevice, reportOut, HID_MAX_PACKET_SIZE, &bytesWritten, &writeOl)) {
        DWORD err = GetLastError();
        if (err != ERROR_IO_PENDING) {
            LOG_HID_ERROR("HID WriteFile failed, error=%d, device=%p, connected=%d", err, g_hDevice, g_isConnected);
            CloseHandle(writeOl.hEvent);
            return -3;
        }
        // 等待写入完成
        DWORD waitResult = WaitForSingleObject(writeOl.hEvent, timeoutMs > 0 ? timeoutMs : 1000);
        if (waitResult == WAIT_TIMEOUT) {
            CancelIo(g_hDevice);
            CloseHandle(writeOl.hEvent);
            return -3;
        }
        if (!GetOverlappedResult(g_hDevice, &writeOl, &bytesWritten, FALSE)) {
            LOG_HID_ERROR("HID WriteFile GetOverlappedResult failed, error=%d", GetLastError());
            CloseHandle(writeOl.hEvent);
            return -3;
        }
    }
    CloseHandle(writeOl.hEvent);

    uint8_t reportIn[HID_MAX_PACKET_SIZE] = {};

    // 使用 OVERLAPPED I/O 进行 ReadFile
    OVERLAPPED readOl = {};
    readOl.hEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    DWORD bytesRead = 0;
    if (!ReadFile(g_hDevice, reportIn, HID_MAX_PACKET_SIZE, &bytesRead, &readOl)) {
        DWORD error = GetLastError();
        if (error != ERROR_IO_PENDING) {
            LOG_HID_ERROR("HID ReadFile failed, error=%d", error);
            CloseHandle(readOl.hEvent);
            return -4;
        }

        DWORD waitResult = WaitForSingleObject(readOl.hEvent, timeoutMs > 0 ? timeoutMs : 1000);
        if (waitResult == WAIT_TIMEOUT) {
            CancelIo(g_hDevice);
            CloseHandle(readOl.hEvent);
            return -5;
        }

        if (!GetOverlappedResult(g_hDevice, &readOl, &bytesRead, FALSE)) {
            LOG_HID_ERROR("HID ReadFile GetOverlappedResult failed, error=%d", GetLastError());
            CloseHandle(readOl.hEvent);
            return -6;
        }
    }
    CloseHandle(readOl.hEvent);
    if (bytesRead == 0) return -7;

    // 响应约定（与上层所有 DAP_* 解析保持一致）：
    //   ReadFile 返回的 IN 报告首字节为 **报告ID**，命令响应从索引 1 开始。
    //   即：resp[0]=报告ID(通常 0x00), resp[1]=命令ID, resp[2..]=响应负载。
    //   实测 orbtrace 固件 V1(HID) 路径为 64 字节 IN 报告且含报告ID前缀，
    //   因此这里原样拷贝（含报告ID），由上层按上述偏移解析。
    //   参考 COMPAT_ANALYSIS.md 第 11.3 节。
    if (resp && respLen) {
        size_t copyLen = std::min((size_t)bytesRead, *respLen);
        memcpy(resp, reportIn, copyLen);
        *respLen = copyLen;

        if (copyLen > 0 && reportIn[0] != CMD_REPORT_ID) {
            LOG_HID_DEBUG("HID response report ID = 0x%02X (expected 0x%02X)",
                          reportIn[0], CMD_REPORT_ID);
        }
    }

    return 0;
}

// ============================================================================
// DAP Command Helpers
// ============================================================================

static int SendDAPCommand(uint8_t cmdId, const uint8_t* data, size_t dataLen)
{
    uint8_t cmd[64] = {};
    cmd[0] = cmdId;
    if (data && dataLen > 0) {
        memcpy(&cmd[1], data, std::min(dataLen, sizeof(cmd) - 1));
    }

    uint8_t resp[64] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, 1 + dataLen, resp, &respLen, 1000);
}

static int DAP_Info(uint8_t infoId, char* buffer, size_t bufferLen)
{
    uint8_t cmd[2] = {ID_DAP_INFO, infoId};
    uint8_t resp[256] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;

    // 响应（含报告ID）：[报告ID][命令ID][Info Length][Info Data...]
    if (buffer && bufferLen > 0) {
        size_t strLen = std::min(static_cast<size_t>(resp[2]), bufferLen - 1);
        memcpy(buffer, &resp[3], strLen);
        buffer[strLen] = '\0';
    }

    return resp[2];
}

static int DAP_ConnectSWD(void)
{
    uint8_t cmd[2] = {ID_DAP_CONNECT, 1};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;

    // 响应（含报告ID）：[报告ID][命令ID][Port]
    return resp[2];
}

static int DAP_SWDConfigure(uint8_t config)
{
    uint8_t cmd[2] = {ID_DAP_SWD_CONFIGURE, config};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
}

static int DAP_TransferConfigure(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry)
{
    uint8_t cmd[6] = {
        ID_DAP_TRANSFER_CONFIG,
        idleCycles,
        static_cast<uint8_t>(waitRetry & 0xFF),
        static_cast<uint8_t>((waitRetry >> 8) & 0xFF),
        static_cast<uint8_t>(matchRetry & 0xFF),
        static_cast<uint8_t>((matchRetry >> 8) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
}

static int DAP_SWJ_Clock(uint32_t clock)
{
    uint8_t cmd[5] = {
        ID_DAP_SWJ_CLOCK,
        static_cast<uint8_t>(clock & 0xFF),
        static_cast<uint8_t>((clock >> 8) & 0xFF),
        static_cast<uint8_t>((clock >> 16) & 0xFF),
        static_cast<uint8_t>((clock >> 24) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
}

// ============================================================================
// Public DAP Functions
// ============================================================================

int DAP_GetInfo(uint8_t infoId, char* buffer, size_t bufferLen) { return DAP_Info(infoId, buffer, bufferLen); }

int DAP_ConnectTarget(void)
{
    int mode = DAP_ConnectSWD();
    if (mode < 0) {
        uint8_t cmd[2] = {ID_DAP_CONNECT, 0};
        uint8_t resp[4] = {};
        size_t respLen = sizeof(resp);
        int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
        if (result != 0 || respLen < 3) return -1;
        mode = resp[2];
    }
    return mode;
}

int DAP_ConnectTargetPort(int port)
{
    // ID_DAP_CONNECT: port 0=默认/自动, 1=SWD, 2=JTAG
    // 响应（含报告ID）：[报告ID][命令ID][端口] = 实际建立的模式
    uint8_t cmd[2] = {ID_DAP_CONNECT, static_cast<uint8_t>(port)};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);

    const int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    return resp[2];
}

int DAP_DisconnectTarget(void) {
    uint8_t cmd[1] = {ID_DAP_DISCONNECT};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, 1, resp, &respLen, 1000);
}
int DAP_ConfigureTransfer(uint8_t idleCycles, uint16_t waitRetry, uint16_t matchRetry) { return DAP_TransferConfigure(idleCycles, waitRetry, matchRetry); }
int DAP_SetSWJClock(uint32_t clock) { return DAP_SWJ_Clock(clock); }
int DAP_ConfigureSWD(uint8_t config) { return DAP_SWDConfigure(config); }

int DAP_Transfer(int dapId, uint8_t request, uint32_t* data)
{
    uint8_t cmd[8] = {ID_DAP_TRANSFER, static_cast<uint8_t>(dapId), 1, request};
    uint8_t resp[64] = {};
    size_t respLen = sizeof(resp);

    if ((request & 0x02) == 0) {
        memcpy(&cmd[4], data, 4);
        int result = ORBMDK_HID_DAPCommand(cmd, 8, resp, &respLen, 1000);
        if (result != 0) {
            LOG_HID_ERROR("DAP_Transfer write failed: result=%d", result);
            return DAP_RES_ERROR;
        }
    } else {
        int result = ORBMDK_HID_DAPCommand(cmd, 4, resp, &respLen, 1000);
        if (result != 0) {
            LOG_HID_ERROR("DAP_Transfer read failed: result=%d", result);
            return DAP_RES_ERROR;
        }
    }

    if (respLen < 4) {
        LOG_HID_ERROR("DAP_Transfer resp too short: respLen=%d", respLen);
        return DAP_RES_ERROR;
    }

    // 标准响应（含报告ID）：
    //   [报告ID][命令ID][Transfer Count][Transfer Response][Transfer Data]
    //   resp[0]=报告ID, resp[1]=命令ID, resp[2]=Transfer Count,
    //   resp[3]=Transfer Response, resp[4..7]=Transfer Data
    // Transfer Response 的 Bit 2..0 是 ACK：1=OK, 2=WAIT, 4=FAULT, 7=NO_ACK
    uint8_t transferResponse = resp[3];
    uint8_t ack = transferResponse & 0x07;
    int status;
    switch (ack) {
        case 1: status = DAP_RES_OK;     break;  // ACK OK
        case 2: status = DAP_RES_WAIT;   break;  // ACK WAIT
        case 4: status = DAP_RES_FAULT;  break;  // ACK FAULT
        case 7: status = DAP_RES_NO_ACK; break;  // ACK NO_ACK
        default: status = DAP_RES_ERROR; break;
    }
    if (transferResponse & 0x10) {  // Bit 4: Value Mismatch
        status = DAP_RES_VALUE_MISMATCH;
    }
    
    if (respLen >= 8) {
        memcpy(data, &resp[4], 4);
    }
    return status;
}

int DAP_TransferBlock(int dapId, uint16_t count, uint8_t request,
                      const uint32_t* writeData, uint32_t* readData)
{
    std::vector<uint8_t> cmd;
    cmd.push_back(ID_DAP_TRANSFER_BLOCK);
    cmd.push_back(static_cast<uint8_t>(dapId));
    cmd.push_back(static_cast<uint8_t>(count & 0xFF));
    cmd.push_back(static_cast<uint8_t>((count >> 8) & 0xFF));
    cmd.push_back(request);

    if (writeData && (request & 0x02) == 0) {
        const uint8_t* data = reinterpret_cast<const uint8_t*>(writeData);
        cmd.insert(cmd.end(), data, data + count * 4);
    }

    // 响应缓冲按**最大可能的块传输**开：V2 Bulk 一次往返可带 ~250 字
    // （5 + 250*4 = 1005 字节），旧值 64*4+4=260 会把 V2 的块传输截断到
    // 63 字，白白浪费了 Bulk 的大包能力。V1 HID 下命令长度本身被下面的
    // HID_MAX_PACKET_SIZE 校验挡住，缓冲开大无副作用。
    uint8_t resp[1024] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp, &respLen, 5000);
    if (result != 0 || respLen < 5) return DAP_RES_ERROR;

    // 标准响应（含报告ID）：
    //   [报告ID][命令ID][Transfer Count LSB][Transfer Count MSB][Transfer Response][Transfer Data]
    //   resp[0]=报告ID, resp[1]=命令ID, resp[2..3]=Transfer Count(2字节),
    //   resp[4]=Transfer Response, resp[5..]=Transfer Data
    uint8_t transferResponse = resp[4];
    uint8_t ack = transferResponse & 0x07;
    int status;
    switch (ack) {
        case 1: status = DAP_RES_OK;     break;  // ACK OK
        case 2: status = DAP_RES_WAIT;   break;  // ACK WAIT
        case 4: status = DAP_RES_FAULT;  break;  // ACK FAULT
        case 7: status = DAP_RES_NO_ACK; break;  // ACK NO_ACK
        default: status = DAP_RES_ERROR; break;
    }
    if (transferResponse & 0x10) {  // Bit 4: Value Mismatch
        status = DAP_RES_VALUE_MISMATCH;
    }
    
    // Transfer Count 校验：固件若未实现 ID_DAP_TRANSFER_BLOCK，可能返回其它
    // 命令的响应，长度够、ACK 也可能凑巧是 OK，会被误判为成功并把垃圾数据
    // 交给调用方。请求数与返回数必须一致，否则按失败处理（调用方会回退到
    // 逐字 DAP_Transfer）。
    const uint16_t respCount = static_cast<uint16_t>(resp[2]) |
                               (static_cast<uint16_t>(resp[3]) << 8);
    if (status == DAP_RES_OK && respCount != count) {
        LOG_HID_ERROR("DAP_TransferBlock: count mismatch (requested=%u, returned=%u)",
                      static_cast<unsigned>(count), static_cast<unsigned>(respCount));
        return DAP_RES_ERROR;
    }

    if (readData && respLen > 5) {
        // 必须按**请求的字数**截断：HID 每次返回整个报告（64/65 字节），
        // (respLen-5)/4 可能比 count 多 1 个字，直接按响应长度 memcpy
        // 会越界写调用方的缓冲区（历史上正是这里踩坏了 AGDI 的数据）。
        size_t availWords = (respLen - 5) / 4;
        if (availWords > count) {
            availWords = count;
        }
        memcpy(readData, &resp[5], availWords * 4);
    }

    return status;
}

int DAP_WriteAbort(int dapId, uint32_t abort)
{
    uint8_t cmd[6] = {
        ID_DAP_WRITE_ABORT, static_cast<uint8_t>(dapId),
        static_cast<uint8_t>(abort & 0xFF),
        static_cast<uint8_t>((abort >> 8) & 0xFF),
        static_cast<uint8_t>((abort >> 16) & 0xFF),
        static_cast<uint8_t>((abort >> 24) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
}

int DAP_SWJ_Sequence(uint8_t count, const uint8_t* data)
{
    std::vector<uint8_t> cmd;
    cmd.push_back(ID_DAP_SWJ_SEQUENCE);
    cmd.push_back(count);
    int nbytes = (count + 7) / 8;
    cmd.insert(cmd.end(), data, data + nbytes);

    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp, &respLen, 1000);
}

int DAP_SWJ_Pins(uint8_t pinSelect, uint8_t pinOut, int* pinIn, int wait)
{
    uint8_t cmd[7] = {
        ID_DAP_SWJ_PINS, pinOut, pinSelect,
        static_cast<uint8_t>(wait & 0xFF),
        static_cast<uint8_t>((wait >> 8) & 0xFF),
        static_cast<uint8_t>((wait >> 16) & 0xFF),
        static_cast<uint8_t>((wait >> 24) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    // 响应（含报告ID）：[报告ID][命令ID][Pin State]
    if (result == 0 && pinIn && respLen >= 3) *pinIn = resp[2];
    return result;
}

int DAP_ResetTarget(void)
{
    LOG_HID_INFO("DAP_ResetTarget: 下发 ID_DAP_RESET_TARGET(0x0A)");
    uint8_t cmd[1] = {ID_DAP_RESET_TARGET};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    const int rc = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 5000);
    LOG_HID_INFO("DAP_ResetTarget: rc=%d, respLen=%u, resp=[%02X %02X %02X %02X]",
                 rc, (unsigned)respLen, resp[0], resp[1], resp[2], resp[3]);
    return rc;
}

int DAP_TransferAbort(int dapId)
{
    uint8_t cmd[2] = {ID_DAP_TRANSFER_ABORT, static_cast<uint8_t>(dapId)};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
}

// ============================================================================
// JTAG Operations
// ============================================================================

int DAP_JTAG_Configure(const uint8_t* irLengths, uint8_t devCount)
{
    // CMSIS-DAP ID_DAP_JTAG_CONFIGURE 请求格式：[0x15][器件数][IR长度0..N-1]
    //
    // ⚠️ 旧实现的形参语义是**反的**（把 irLength 当成"器件数"发出去，把 devCount
    // 当成"IR 长度"）—— 因为 JTAG 通路从来没被调用过，这个错一直没暴露。
    // 实现 JTAG（§18.9）时必须按规范来，否则探针算不出 IR/DR 序列。
    if (!irLengths || devCount == 0 || devCount > 16) {
        return -1;
    }

    uint8_t cmd[2 + 16];
    cmd[0] = ID_DAP_JTAG_CONFIGURE;
    cmd[1] = devCount;
    memcpy(&cmd[2], irLengths, devCount);

    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    const int result = ORBMDK_HID_DAPCommand(cmd, (size_t)(2 + devCount), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    // 响应格式：[报告ID][命令ID][状态]
    return resp[2] == 0 ? 0 : -1;
}

int DAP_JTAG_Sequence(const JtagSeg* segs, int segCount,
                      uint8_t* tdoOut, size_t tdoCap, size_t* tdoLen)
{
    if (tdoLen) *tdoLen = 0;
    if (!segs || segCount <= 0) return -1;

    // 线上格式：[0x14][段数][info][TDI…]…
    //   info = 位数(1..63；0 表示 64) | bit6 = TMS 电平 | bit7 = 捕获 TDO
    // 固件侧 DAP.c:DAP_JTAG_Sequence() 逐段取出 info 与 (bits+7)/8 字节 TDI。
    std::vector<uint8_t> cmd;
    cmd.reserve(2 + static_cast<size_t>(segCount) * 10);
    cmd.push_back(ID_DAP_JTAG_SEQUENCE);
    cmd.push_back(static_cast<uint8_t>(segCount));

    for (int i = 0; i < segCount; ++i) {
        const JtagSeg& s = segs[i];
        const int bits = (s.bits == 0 || s.bits > 64) ? 64 : s.bits;
        const uint8_t info = static_cast<uint8_t>((bits & 0x3F) |
                                                  (s.tms ? 0x40u : 0x00u) |
                                                  (s.capture ? 0x80u : 0x00u));
        cmd.push_back(info);

        const size_t nbytes = static_cast<size_t>((bits + 7) / 8);
        if (s.tdi) {
            cmd.insert(cmd.end(), s.tdi, s.tdi + nbytes);
        } else {
            cmd.insert(cmd.end(), nbytes, 0);   // 未给 TDI 时按全 0 发送
        }
    }

    uint8_t resp[128] = {};
    size_t respLen = sizeof(resp);
    const int result = ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp, &respLen, 1000);
    if (result != 0) return -1;

    // 响应（已规整为 V1 布局）：[报告ID][0x14][status][捕获段的 TDO 依次拼接]
    //   旧实现漏了 status，直接从 resp[2] 取 TDO —— 每个捕获字节都错位一字节，
    //   这正是"JTAG 扫链恒为空"的直接原因之一（见 COMPAT_ANALYSIS §18.9）。
    if (respLen < 3 || resp[1] != ID_DAP_JTAG_SEQUENCE) return -1;
    if (resp[2] != 0) return -2;   // 固件拒绝（DAP_ERROR，如参数非法）

    const size_t avail = respLen - 3;
    if (tdoOut && tdoCap > 0 && avail > 0) {
        const size_t n = (avail < tdoCap) ? avail : tdoCap;
        memcpy(tdoOut, &resp[3], n);
        if (tdoLen) *tdoLen = n;
    }
    return 0;
}

int DAP_JTAG_IDCODE(int* idcodeCount, uint32_t* idcodes)
{
    uint8_t cmd[1] = {ID_DAP_JTAG_IDCODE};
    uint8_t resp[64] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 2) return -1;

    // 响应（含报告ID）：[报告ID][命令ID][IDCODE Count][IDCODE 数据...]
    if (idcodeCount) {
        *idcodeCount = resp[2];
    }

    if (idcodes && respLen > 3) {
        size_t idcodeCount_local = resp[2];
        for (size_t i = 0; i < idcodeCount_local && (i * 4 + 7) <= respLen; i++) {
            idcodes[i] = resp[3 + i * 4] |
                        ((uint32_t)resp[4 + i * 4] << 8) |
                        ((uint32_t)resp[5 + i * 4] << 16) |
                        ((uint32_t)resp[6 + i * 4] << 24);
        }
    }

    return 0;
}

// ============================================================================
// SWO Trace Operations
// ============================================================================

int DAP_SWO_Transport(uint8_t transport)
{
    uint8_t cmd[2] = {ID_DAP_SWO_TRANSPORT, transport};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    return resp[2] == 0 ? 0 : -1;
}

int DAP_SWO_Mode(uint8_t mode)
{
    uint8_t cmd[2] = {ID_DAP_SWO_MODE, mode};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    return resp[2] == 0 ? 0 : -1;
}

int DAP_SWO_Baudrate(uint32_t baudrate)
{
    uint8_t cmd[5] = {
        ID_DAP_SWO_BAUDRATE,
        static_cast<uint8_t>(baudrate & 0xFF),
        static_cast<uint8_t>((baudrate >> 8) & 0xFF),
        static_cast<uint8_t>((baudrate >> 16) & 0xFF),
        static_cast<uint8_t>((baudrate >> 24) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    return resp[2] == 0 ? 0 : -1;
}

int DAP_SWO_Control(uint8_t control, uint8_t* status)
{
    uint8_t cmd[2] = {ID_DAP_SWO_CONTROL, control};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    // 响应（含报告ID）：[报告ID][命令ID][状态]
    if (status) *status = resp[2];
    return 0;
}

int DAP_SWO_Data(uint8_t* data, size_t* dataLen, uint8_t* status, uint16_t* traceCount)
{
    if (!dataLen) return -1;

    uint8_t cmd[3] = {
        ID_DAP_SWO_DATA,
        static_cast<uint8_t>((*dataLen) & 0xFF),
        static_cast<uint8_t>(((*dataLen) >> 8) & 0xFF),
    };
    uint8_t resp[512] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;

    // 响应格式：[报告ID][命令ID][状态][计数LSB][计数MSB][数据...]
    if (status) *status = resp[2];

    if (traceCount) {
        *traceCount = resp[3] | ((uint16_t)resp[4] << 8);
    }

    // Data starts at byte 5
    if (data && respLen > 5) {
        size_t copyLen = std::min(*dataLen, respLen - 5);
        memcpy(data, &resp[5], copyLen);
        *dataLen = copyLen;
    } else {
        *dataLen = 0;
    }

    return 0;
}

// ============================================================================
// Command Queue Operations
// ============================================================================

int DAP_QueueCommands(const uint8_t* commands, size_t cmdLen, uint8_t* responses, size_t* respLen)
{
    if (!commands || !respLen) return -1;

    std::vector<uint8_t> cmd;
    cmd.push_back(ID_DAP_QUEUE_COMMANDS);
    cmd.insert(cmd.end(), commands, commands + cmdLen);

    std::vector<uint8_t> resp(*respLen > 0 ? *respLen : 256);
    size_t localRespLen = resp.size();

    int result = ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp.data(), &localRespLen, 5000);
    if (result != 0 || localRespLen < 2) return -1;

    if (responses && *respLen > 0) {
        size_t copyLen = std::min(*respLen, localRespLen);
        memcpy(responses, resp.data(), copyLen);
    }
    *respLen = localRespLen;

    return resp[1] == ID_DAP_QUEUE_COMMANDS ? 0 : -1;
}

// ============================================================================
// DAP Advanced Operations
// ============================================================================

int DAP_Delay(uint16_t delay_us)
{
    uint8_t cmd[3] = {
        ID_DAP_DELAY,
        static_cast<uint8_t>(delay_us & 0xFF),
        static_cast<uint8_t>((delay_us >> 8) & 0xFF),
    };
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, delay_us + 100);
    if (result != 0 || respLen < 3) return -1;
    // 响应（含报告ID）：[报告ID][命令ID][状态]
    return resp[2] == 0 ? 0 : -1;
}

int DAP_SWD_Sequence(uint8_t count, const uint8_t* data)
{
    if (count == 0) return 0;

    std::vector<uint8_t> cmd;
    cmd.push_back(ID_DAP_SWD_SEQUENCE);
    cmd.push_back(count);
    size_t nbytes = (count + 7) / 8;
    if (data) {
        cmd.insert(cmd.end(), data, data + nbytes);
    } else {
        cmd.insert(cmd.end(), nbytes, 0);
    }

    uint8_t resp[64] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    // 响应格式：[报告ID][命令ID][状态]
    return resp[2] == 0 ? 0 : -1;
}

int DAP_HostStatus(uint8_t hostStatus, uint8_t state)
{
    uint8_t cmd[3] = {ID_DAP_HOST_STATUS, hostStatus, state};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
    if (result != 0 || respLen < 3) return -1;
    // 响应格式：[报告ID][命令ID][状态]
    return resp[2] == 0 ? 0 : -1;
}

int DAP_GetSupportedOptimisationLevel(void)
{
    // 返回支持的优化级别
    // 0 = 无优化，所有操作串行执行
    return 0;
}

static int g_commTimeoutMs = 1000;

int DAP_SetCommTimeout(int timeoutMs)
{
    if (timeoutMs > 0 && timeoutMs <= 30000) {
        g_commTimeoutMs = timeoutMs;
    }
    return 0;
}

int DAP_TargetOp(int operation, int* result)
{
    int ret = 0;

    switch (operation) {
        case 0:  // Halt
            // 发送 Halt 请求
            *result = 0;
            break;
        case 1:  // Resume
            *result = 0;
            break;
        case 2:  // Step
            *result = 0;
            break;
        case 3:  // Reset
            LOG_HID_INFO("DAP_TargetOp: operation=Reset, 调用 DAP_ResetTarget");
            ret = DAP_ResetTarget();
            *result = (ret == 0) ? 0 : -1;
            LOG_HID_INFO("DAP_TargetOp: DAP_ResetTarget rc=%d, result=%d", ret, *result);
            break;
        default:
            return -1;
    }
    return 0;
}

// ============================================================================
// Streaming Trace Operations
// ============================================================================

static std::mutex               g_traceMutex;
static std::condition_variable  g_traceCv;
static bool                     g_traceInitialized = false;
static bool                     g_traceRunning     = false;
static bool                     g_traceStopThread  = false;
static std::thread              g_traceThread;
static std::vector<uint8_t>     g_traceRing;                  // 环形缓冲
static size_t                   g_traceHead = 0;              // 写指针
static size_t                   g_traceTail = 0;              // 读指针
static size_t                   g_traceBytes = 0;             // 已缓存字节数
static uint32_t                 g_traceBaudrate = 2000000;    // 由 ConfigureDebugger 的 TraceBaudrate= 更新
static uint8_t                  g_traceSwoPort  = 1;          // SWO 端口：1 = UART/NRZ，2 = Manchester
                                                              // 由 ConfigureDebugger 的 Trace= 解析后经 StreamingTrace_SetMode 设定
static uint32_t                 g_traceDroppedBytes = 0;      // 缓冲满丢弃统计
static const size_t             kTraceBufferSize = 512 * 1024;
static const size_t             kTraceReadChunk  = 512;

// 调用方必须持有 g_traceMutex
static void TraceRingReset()
{
    g_traceHead = g_traceTail = g_traceBytes = 0;
    g_traceDroppedBytes = 0;
}

// 调用方必须持有 g_traceMutex
static void TraceRingPush(const uint8_t* data, size_t len)
{
    if (g_traceRing.empty()) return;
    const size_t cap = g_traceRing.size();
    for (size_t i = 0; i < len; ++i) {
        if (g_traceBytes == cap) {
            // 满：丢最旧的一个字节，宁可丢数据也不把 AGDI 的等待卡死
            g_traceTail = (g_traceTail + 1) % cap;
            --g_traceBytes;
            ++g_traceDroppedBytes;
        }
        g_traceRing[g_traceHead] = data[i];
        g_traceHead = (g_traceHead + 1) % cap;
        ++g_traceBytes;
    }
}

// 调用方必须持有 g_traceMutex；返回实际取出的字节数
static size_t TraceRingPop(uint8_t* out, size_t maxLen)
{
    if (g_traceRing.empty()) return 0;
    const size_t cap = g_traceRing.size();
    size_t n = (maxLen < g_traceBytes) ? maxLen : g_traceBytes;
    for (size_t i = 0; i < n; ++i) {
        out[i] = g_traceRing[g_traceTail];
        g_traceTail = (g_traceTail + 1) % cap;
    }
    g_traceBytes -= n;
    return n;
}

// 后台读取线程（兜底方案 B）：轮询 ID_DAP_SWO_DATA 把数据搬进环形缓冲。
// 与 AGDI 的调试命令走同一条 HID/V2 通道，通道内部已串行化，因此是"抢总线"而非"撕数据"。
static void TraceReadThreadFunc()
{
    uint8_t local[kTraceReadChunk];
    LOG_HID_INFO("TraceReadThreadFunc: SWO 读取线程启动");
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(g_traceMutex);
            if (g_traceStopThread) break;
        }

        size_t len = sizeof(local);
        uint8_t status = 0;
        uint16_t traceCount = 0;
        int rc = DAP_SWO_Data(local, &len, &status, &traceCount);
        if (rc == 0 && len > 0) {
            std::lock_guard<std::mutex> lock(g_traceMutex);
            TraceRingPush(local, len);
            g_traceCv.notify_all();
            continue;
        }

        // 无数据：小睡，避免空转刷 USB
        std::unique_lock<std::mutex> lock(g_traceMutex);
        g_traceCv.wait_for(lock, std::chrono::milliseconds(2),
                           [] { return g_traceStopThread; });
    }
    LOG_HID_INFO("TraceReadThreadFunc: SWO 读取线程退出（丢弃 %u 字节）", g_traceDroppedBytes);
}

int StreamingTrace_Init(void)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (g_traceInitialized) return 0;

    g_traceRing.assign(kTraceBufferSize, 0);
    TraceRingReset();
    g_traceInitialized = true;
    g_traceRunning     = false;
    g_traceStopThread  = false;
    return 0;
}

void StreamingTrace_Shutdown(void)
{
    StreamingTrace_Stop();
    std::lock_guard<std::mutex> lock(g_traceMutex);
    TraceRingReset();
    g_traceRing.clear();
    g_traceInitialized = false;
}

// AGDI 的 Start(handle, sinkIndex) 传的是 sink 下标（0 = cmsis_dap_swo_trace），不是 mode；
// RDDI 层负责把 sink 下标翻成这里的 mode（0 = none, 1 = SWO, 2 = ETM）。
int StreamingTrace_Start(uint8_t mode)
{
    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        if (!g_traceInitialized) {
            g_traceRing.assign(kTraceBufferSize, 0);
            TraceRingReset();
            g_traceInitialized = true;
        }
        if (g_traceRunning) return 0;
    }

    if (mode != 1) {
        // ETM / 未知模式：明确失败，不假成功
        LOG_HID_INFO("StreamingTrace_Start: mode=%u 不支持（本层只有 SWO=1）", (unsigned)mode);
        return -1;
    }

    // 完整上电序列：Transport → Mode → Baudrate → Control(Start)
    // （旧实现漏了 Mode，且 Baudrate 从未下发 —— §18.10-C 阶段 2/3）
    if (DAP_SWO_Transport(1) != 0) {                 // 1 = SWO
        LOG_HID_ERROR("StreamingTrace_Start: DAP_SWO_Transport(1) 失败");
        return -1;
    }
    if (DAP_SWO_Mode(g_traceSwoPort) != 0) {         // 1 = UART/NRZ，2 = Manchester（按用户选择）
        LOG_HID_ERROR("StreamingTrace_Start: DAP_SWO_Mode(%u) 失败", (unsigned)g_traceSwoPort);
        return -1;
    }
    if (DAP_SWO_Baudrate(g_traceBaudrate) != 0) {
        LOG_HID_ERROR("StreamingTrace_Start: DAP_SWO_Baudrate(%u) 失败", g_traceBaudrate);
        return -1;
    }
    uint8_t swoStatus = 0;
    if (DAP_SWO_Control(1, &swoStatus) != 0) {       // 1 = Start
        LOG_HID_ERROR("StreamingTrace_Start: DAP_SWO_Control(Start) 失败");
        return -1;
    }

    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        g_traceRunning    = true;
        g_traceStopThread = false;
        g_traceThread     = std::thread(TraceReadThreadFunc);
    }
    LOG_HID_INFO("StreamingTrace_Start: SWO 流式启动（baud=%u, SWO status=0x%02X）",
                 g_traceBaudrate, swoStatus);
    return 0;
}

int StreamingTrace_Stop(void)
{
    std::thread worker;
    {
        std::lock_guard<std::mutex> lock(g_traceMutex);
        if (!g_traceRunning && !g_traceThread.joinable()) return 0;
        g_traceRunning    = false;
        g_traceStopThread = true;
        g_traceCv.notify_all();
        worker = std::move(g_traceThread);
    }
    if (worker.joinable()) worker.join();

    uint8_t swoStatus = 0;
    DAP_SWO_Control(0, &swoStatus);                  // 0 = Stop
    return 0;
}

// 阻塞式取数据（AGDI 的 WaitForEvent 语义靠它）：
//   返回值 >0 = 取到的字节数；0 = 超时（无数据）；-1 = 未在运行或参数错
// 关键：到了 timeoutMs 必须**立刻**返回，而不是继续轮询刷总线。
// 数据本身由 TraceReadThreadFunc 后台搬进环形缓冲，这里只做消费。
int StreamingTrace_Read(uint8_t* buffer, size_t* size, uint32_t timeoutMs)
{
    if (!buffer || !size || *size == 0) return -1;
    const size_t want = *size;

    std::unique_lock<std::mutex> lock(g_traceMutex);
    if (!g_traceRunning) { *size = 0; return -1; }

    if (g_traceBytes == 0 && timeoutMs > 0) {
        g_traceCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [] { return g_traceBytes > 0 || !g_traceRunning; });
    }

    size_t n = TraceRingPop(buffer, want);
    *size = n;
    return (int)n;
}

// 旧接口保留：语义与 StreamingTrace_Read 完全一致（旧实现是"最多 512 字节一轮的忙等"）
int StreamingTrace_GetData(uint8_t* buffer, size_t* size, uint32_t timeoutMs)
{
    return StreamingTrace_Read(buffer, size, timeoutMs);
}

// 清空环形缓冲（AGDI 的 Flush(handle, sinkIndex) 落到这里）
int StreamingTrace_Flush(void)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    TraceRingReset();
    return 0;
}

// 由 RDDI 层在 ConfigureDebugger 解析 TraceBaudrate= 后调用（真正下发在 Start）
void StreamingTrace_SetBaudrate(uint32_t baudrate)
{
    if (baudrate == 0) return;
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_traceBaudrate = baudrate;
}

// 设定 SWO 端口：1 = UART/NRZ，2 = Manchester。
// 两个时机上的约束（写在这里以免后来者随手挪动）：
//   1) 必须早于 StreamingTrace_Start —— Start 会按它下发 DAP_SWO_Mode；
//   2) 影响 StreamingTrace_GetSinkInfo 回报的类型串 —— AGDI 拿到"SWO-Manchester"
//      之后才会把 `Trace=SWO-Manchester` 写进配置串，前后必须自洽。
void StreamingTrace_SetMode(uint8_t swoPort)
{
    if (swoPort != 1 && swoPort != 2) {
        LOG_HID_INFO("StreamingTrace_SetMode: 忽略非法端口 %u（只接受 1=UART / 2=Manchester）",
                     (unsigned)swoPort);
        return;
    }
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_traceSwoPort = swoPort;
}

int StreamingTrace_GetStatus(uint8_t* status, uint16_t* traceCount)
{
    if (!status) return -1;

    uint8_t cmd[1] = {ID_DAP_SWO_STATUS};
    uint8_t resp[8] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
    // 响应（含报告ID）：[报告ID][命令ID][Status][Count LSB][Count MSB]
    //   原实现按 resp[1]/resp[2]/resp[3] 解析，漏掉了报告ID偏移，导致读到命令ID。
    if (result != 0 || respLen < 5) {
        *status = 0;
        if (traceCount) *traceCount = 0;
        return -1;
    }

    *status = resp[2];
    if (traceCount) {
        *traceCount = (uint16_t)(resp[3] | ((uint16_t)resp[4] << 8));
    }
    return 0;
}

int StreamingTrace_GetSinkInfo(int index, char* name, int nameLen, char* type, int typeLen)
{
    // 名称/类型必须与 AGDI 及官方 CMSIS_DAP.dll 里的字面量一致（反汇编确认）：
    //   - "cmsis_dap_swo_trace" 同时出现在 AGDI 与官方 DLL 中（sink 名）
    //   - AGDI 的流式配置串是 Trace=SWO-UART;TraceTransport=Stream;（类型为 SWO-UART）
    // 旧实现返回 "SWO"/"UART"/"ETM"，既不是 sink 名也不是类型名，AGDI 匹配不上。
    // 本层只有 1 个 sink：ETM 已证不可达（§18.10-B / §18.9），故只接受 index == 0。
    if (index != 0) return -1;

    if (name && nameLen > 0) strncpy_s(name, nameLen, "cmsis_dap_swo_trace", nameLen - 1);
    // 类型串必须和用户实际选中的端口一致。AGDI 会把这里回报的类型拼进
    // `Trace=<类型>;TraceTransport=Stream;` 再交给 ConfigureDebugger；
    // 若此处恒报 "SWO-UART"，那么"用户选了 Manchester"会被这条串改写成 UART，
    // 界面选择与本层实际下发的配置就互相矛盾了。
    if (type && typeLen > 0) {
        const char* typeStr = (g_traceSwoPort == 2) ? "SWO-Manchester" : "SWO-UART";
        strncpy_s(type, typeLen, typeStr, typeLen - 1);
    }
    return 0;
}

} // namespace ORBMDK
