/**
 * @file ORBMDK_USB_Bulk.cpp
 * @brief USB Bulk 传输层实现
 *
 * 支持 CMSIS-DAP V1 (HID) 和 V2 (USB Bulk/WinUSB) 模式
 */

#include "pch.h"
#include "ORBMDK.h"
#include "ORBMDK_USB_Bulk.h"
#include "ORBMDK_HID.h"
#include "ORBMDK_DAPV2.h"

#include <chrono>
#include <thread>

// Windows SDK 头文件 (WinUSB 传输)
#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>
#pragma comment(lib, "setupapi.lib")

// WinUSB 需要在 setupapi.h 之后包含，且需要先定义
#define WINUSB_NO_DEFAULT_LIB
#include <winusb.h>
#pragma comment(lib, "winusb.lib")

// GUID for USB devices (定义在 WinUSB 辅助函数区)
using namespace ORBMDK;

// ============================================================================
// 内部常量
// ============================================================================

// ORBTrace 设备 VID/PID
// 统一使用 ORBMDK.h 中的常量，避免与 HID 层 (ORBMDK_HID.cpp) 不一致。
// 历史问题：此处曾为 0x6A02，而 HID 层为 0x3456，导致 HID/Bulk 匹配到不同设备。
#define ORBTRACE_VID         ORBMDK_ORBTRACE_VID
#define ORBTRACE_PID         ORBMDK_ORBTRACE_PID

// 默认超时 (ms)
#define DEFAULT_BULK_TIMEOUT 1000

// ============================================================================
// WinUSB 内部上下文
// ============================================================================

struct WinUSBContext {
    HANDLE deviceHandle;           // 设备文件句柄
    WINUSB_INTERFACE_HANDLE winusbHandle;  // WinUSB 接口句柄
    UCHAR bulkOutPipe;            // Bulk OUT 端点
    UCHAR bulkInPipe;             // Bulk IN 端点
    bool initialized;
};

static WinUSBContext g_winusb = {};

// ============================================================================
// USB Bulk 内部上下文
// ============================================================================

struct USB_Bulk_Context {
    bool initialized;
    bool hidAvailable;
    USB_Bulk_Mode mode;
    int timeoutMs;
    uint32_t timerFrequency;
    bool timerStarted;
    std::chrono::steady_clock::time_point timerStart;
    USB_Bulk_Device_Info deviceInfo;
};

static USB_Bulk_Context g_ctx = {};

// ============================================================================
// WinUSB 辅助函数
// ============================================================================

// CMSIS-DAP v2 的 WinUSB 接口 GUID —— **枚举 V2 必须用这个**。
//
// 实测根因：V2 接口（复合设备的 MI_05）注册的**不是**
// GUID_DEVINTERFACE_USB_DEVICE({A5DCBF10-...})。用后者枚举只能拿到复合设备的
// **父节点** `\\?\usb#vid_1209&pid_3443#<serial>#...`（路径里没有 `&mi_XX`），
// 对父节点 WinUsb_Initialize 必然失败(err=8)，于是永远找不到 V2 —— 表现为
// Keil 对话框里只有 "CMSIS-DAP v1"。
//
// 该 GUID 是 CMSIS-DAP v2 规范规定的接口 GUID；官方 CMSIS_DAP.dll 的 .rdata
// 偏移 0x41DE5 处就是这个 GUID 的字节（已用字节比对确认）。
static const GUID CMSIS_DAP_V2_GUID_DEVINTERFACE =
    {0xCDB3B5AD, 0x293B, 0x4663, {0xAA, 0x36, 0x1A, 0xAE, 0x46, 0x46, 0x37, 0x76}};

// 标准 WinUSB 设备接口 GUID（UsbTreeView 里标记为 GUID_DEVINTERFACE_WINUSB）。
//
// orbtrace 的 MI_05("CMSIS-DAP v2") 同时挂了两个接口 GUID：本 GUID 与上面的
// CMSIS-DAP 专用 GUID。实测用专用 GUID 调 SetupDiGetClassDevs 枚举到 0 个接口
// （尽管注册表 DeviceClasses 下确实有该实例），所以优先用这个标准的 WinUSB GUID。
static const GUID WINUSB_GUID_DEVINTERFACE =
    {0xDEE824EF, 0x729B, 0x4A0E, {0x9C, 0x14, 0xB7, 0x11, 0x7D, 0x33, 0xA8, 0x17}};

// 普通 USB 设备接口 GUID (从 wdmguid.h)，作为兜底再试一次
static const GUID USB_GUID_DEVINTERFACE = 
    {0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED}};

// ---------------------------------------------------------------------------
// 诊断日志
//
// 本文件此前完全没有日志（见 COMPAT_ANALYSIS.md §8.1），V2 打不开时无从排查。
// 这里只记关键路径（枚举到的接口、被拒绝的原因、最终选中的端点），
// 不记逐次传输。写入与 RDDI 层相同的日志文件。
// ---------------------------------------------------------------------------
static void BulkTrace(const char* fmt, ...)
{
    char path[MAX_PATH] = {0};
    char tmp[MAX_PATH] = {0};
    if (GetTempPathA(MAX_PATH, tmp) == 0 || tmp[0] == '\0') {
        return;
    }
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%sORBMDK_RDDI.log", tmp);

    char msg[512] = {0};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0) {
        return;
    }
    fprintf(f, "[ORBMDK][INFO][BULK] %s\n", msg);
    fclose(f);
}

// V2 接口的产品名（来自接口字符串描述符，通常是 "CMSIS-DAP v2"）
static char g_bulkProductName[128] = {0};

/**
 * @brief 读取接口字符串描述符
 *
 * 为什么要单独读：Keil 对话框里的适配器名字就是 CMSIS_DAP_Identify(idNo=2)
 * 返回的产品名。V1 时它来自 HidD_GetProductString（HID 接口）→ "CMSIS-DAP v1"；
 * V2 必须改从 **Bulk 接口自己的字符串描述符** 读，否则即使走的是 V2 传输，
 * 对话框仍显示 "CMSIS-DAP v1"。
 *
 * 注："CMSIS-DAP v" 这个字面量在 AGDI 和官方 RDDI DLL 里都**不存在**
 * （已用字节搜索确认），所以该名字只能来自设备描述符。
 */
static void _readInterfaceString(WINUSB_INTERFACE_HANDLE h, UCHAR iInterface)
{
    g_bulkProductName[0] = '\0';

    if (iInterface == 0) {
        BulkTrace("interface has no string descriptor (iInterface=0)");
        return;
    }

    // 字符串描述符最长 255 字节（2 字节头 + UTF-16LE 正文）
    UCHAR buf[256] = {0};
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, iInterface, 0x0409,
                              buf, sizeof(buf), &got) || got < 2) {
        BulkTrace("interface string %u unavailable (got=%lu)",
                  (unsigned)iInterface, (unsigned long)got);
        return;
    }

    const int chars = (int)((got - 2) / 2);
    const int n = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(buf + 2),
                                      chars, g_bulkProductName,
                                      (int)sizeof(g_bulkProductName) - 1,
                                      nullptr, nullptr);
    g_bulkProductName[(n > 0) ? n : 0] = '\0';
    BulkTrace("interface string %u = '%s'", (unsigned)iInterface, g_bulkProductName);
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetProductName(char* buf, size_t len)
{
    if (!buf || len == 0) {
        return -1;
    }
    buf[0] = '\0';
    if (g_bulkProductName[0] == '\0') {
        return -1;
    }
    strncpy_s(buf, len, g_bulkProductName, _TRUNCATE);
    return 0;
}

/**
 * @brief 从设备接口路径解析 VID/PID
 *
 * 路径形如：\\?\usb#vid_1209&pid_3443&mi_01#7&1a2b3c4d&0&0001#{...}
 *
 * 不用 WinUsb_GetDescriptor 取设备描述符的原因：在**复合设备的接口**上请求
 * "设备描述符"通常直接失败，旧实现因此在 continue 处跳过，永远打不开 V2。
 */
static bool _parseVidPidFromPath(const char* path, uint16_t* vid, uint16_t* pid)
{
    if (!path || !vid || !pid) {
        return false;
    }

    // 大小写都可能出现，统一按小写比较
    char lower[MAX_PATH] = {0};
    strncpy_s(lower, sizeof(lower), path, _TRUNCATE);
    for (char* c = lower; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char)(*c - 'A' + 'a');
        }
    }

    const char* v = strstr(lower, "vid_");
    const char* p = strstr(lower, "pid_");
    if (!v || !p) {
        return false;
    }

    *vid = (uint16_t)strtoul(v + 4, nullptr, 16);
    *pid = (uint16_t)strtoul(p + 4, nullptr, 16);
    return true;
}

/**
 * @brief 在指定接口 GUID 下查找并打开目标设备
 *
 * @param guid          设备接口 GUID（V2 必须用 CMSIS-DAP 专用 GUID）
 * @param candidatesOut 累计已通过 VID/PID 路径过滤的候选接口数
 */
static bool _findAndOpenDeviceInGuid(const GUID* guid, uint16_t vid, uint16_t pid,
                                     int* candidatesOut)
{
    BOOL result = FALSE;
    HDEVINFO deviceInfoSet;
    SP_DEVICE_INTERFACE_DATA deviceInterfaceData;
    PSP_DEVICE_INTERFACE_DETAIL_DATA deviceDetailData = NULL;
    DWORD detailSize = 0;
    DWORD lastError = 0;
    bool deviceOpened = false;
    int  candidates = 0;

    // 构建设备接口查询
    deviceInfoSet = SetupDiGetClassDevs(guid,
                                        NULL,
                                        NULL,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

    if (deviceInfoSet == INVALID_HANDLE_VALUE) {
        BulkTrace("SetupDiGetClassDevs failed, err=%lu", (unsigned long)GetLastError());
        return false;
    }

    // 枚举设备接口
    deviceInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);

    // 注意：这里的 GUID 必须与上面 SetupDiGetClassDevs 传入的**完全一致**。
    // 传不一致的 GUID 时 SetupDiEnumDeviceInterfaces 会枚举不到任何东西，
    // 而且不返回任何错误 —— 这正是之前 WinUSB / CMSIS-DAPv2 两个 GUID 都
    // "枚举到 0 个候选"的原因（当时这里还硬编码着 USB_GUID_DEVINTERFACE，
    // 只有第三轮碰巧一致才拿到候选）。独立测试程序传同一 GUID 则完全正常。
    for (DWORD index = 0; SetupDiEnumDeviceInterfaces(deviceInfoSet, NULL,
                guid, index, &deviceInterfaceData); index++) {

        // 获取设备详情大小
        SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData,
                                       NULL, 0, &detailSize, NULL);

        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            continue;
        }

        // 分配详情结构
        deviceDetailData = (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(detailSize);
        if (!deviceDetailData) {
            continue;
        }

        deviceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        // 获取设备路径
        if (!SetupDiGetDeviceInterfaceDetail(deviceInfoSet, &deviceInterfaceData,
                                             deviceDetailData, detailSize, NULL, NULL)) {
            free(deviceDetailData);
            continue;
        }

        // ---- 1) 先用设备路径过滤 VID/PID ----
        // 必须在 CreateFile / WinUsb_Initialize **之前**做：系统里有几十上百个
        // USB 设备接口，对无关设备逐个 CreateFile 既慢又必然失败，而且这些失败
        // 路径原本没有任何日志，现场完全看不出"到底卡在哪一步"。
        // 路径形如 \\?\usb#vid_1209&pid_3443&mi_01#...
        uint16_t pathVid = 0, pathPid = 0;
        if (!_parseVidPidFromPath(deviceDetailData->DevicePath, &pathVid, &pathPid) ||
            pathVid != vid || pathPid != pid) {
            free(deviceDetailData);
            continue;
        }
        ++candidates;
        BulkTrace("candidate[%d] '%s'", candidates, deviceDetailData->DevicePath);

        // ---- 2) 打开设备 ----
        HANDLE hDevice = CreateFile(deviceDetailData->DevicePath,
                                    GENERIC_WRITE | GENERIC_READ,
                                    FILE_SHARE_WRITE | FILE_SHARE_READ,
                                    NULL,
                                    OPEN_EXISTING,
                                    FILE_FLAG_OVERLAPPED,
                                    NULL);

        // WinUSB 设备是**独占**的。AGDI 在一次会话里会反复 rddi_Open/Close，
        // 上一个句柄刚关闭、系统尚未完全释放时，这里会瞬时返回
        // ERROR_ACCESS_DENIED(5)。不重试的话整场会话都会退化成 V1
        // （实测：7 次打开只有第 1 次走 V2，其余 6 次全是 HID）。
        DWORD openErr = GetLastError();
        if (hDevice == INVALID_HANDLE_VALUE && openErr == ERROR_ACCESS_DENIED) {
            for (int retry = 0; retry < 5 && hDevice == INVALID_HANDLE_VALUE; ++retry) {
                Sleep(100);
                hDevice = CreateFile(deviceDetailData->DevicePath,
                                     GENERIC_WRITE | GENERIC_READ,
                                     FILE_SHARE_WRITE | FILE_SHARE_READ,
                                     NULL,
                                     OPEN_EXISTING,
                                     FILE_FLAG_OVERLAPPED,
                                     NULL);
                openErr = GetLastError();
            }
            if (hDevice != INVALID_HANDLE_VALUE) {
                BulkTrace("  CreateFile succeeded after retry");
            }
        }

        if (hDevice == INVALID_HANDLE_VALUE) {
            BulkTrace("  reject: CreateFile failed, err=%lu",
                      (unsigned long)openErr);
            free(deviceDetailData);
            continue;
        }

        // ---- 3) 绑定 WinUSB ----
        WINUSB_INTERFACE_HANDLE tempWinUSB;
        if (!WinUsb_Initialize(hDevice, &tempWinUSB)) {
            // 最常见的原因：该接口没有绑定 WinUSB 驱动（是 HID / 未装驱动）。
            BulkTrace("  reject: WinUsb_Initialize failed, err=%lu "
                      "(interface not bound to WinUSB?)", (unsigned long)GetLastError());
            CloseHandle(hDevice);
            free(deviceDetailData);
            continue;
        }

        // ---- 4) 接口描述符 ----
        USB_INTERFACE_DESCRIPTOR ifaceDesc;
        if (!WinUsb_QueryInterfaceSettings(tempWinUSB, 0, &ifaceDesc)) {
            BulkTrace("  reject: WinUsb_QueryInterfaceSettings failed, err=%lu",
                      (unsigned long)GetLastError());
            WinUsb_Free(tempWinUSB);
            CloseHandle(hDevice);
            free(deviceDetailData);
            continue;
        }

        // ---- 接口类型校验 ----
        // 同一 VID/PID 下复合设备会暴露多个接口：HID 接口(class 3) 在前、
        // CMSIS-DAP V2 的 Bulk 接口(class 0xFF vendor-specific) 在后。
        // 不校验的话会先匹配到 HID 接口并 break —— 而它没有任何 Bulk 端点。
        if (ifaceDesc.bInterfaceClass != 0xFF) {
            BulkTrace("skip iface %u of vid_%04X&pid_%04X: class=0x%02X (need 0xFF vendor-specific)",
                      (unsigned)ifaceDesc.bInterfaceNumber, vid, pid,
                      (unsigned)ifaceDesc.bInterfaceClass);
            WinUsb_Free(tempWinUSB);
            CloseHandle(hDevice);
            free(deviceDetailData);
            continue;
        }

        // ---- 端点发现：必须同时具备 Bulk IN 与 Bulk OUT ----
        UCHAR inPipe = 0, outPipe = 0;
        for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(tempWinUSB, 0, i, &pipeInfo) &&
                pipeInfo.PipeType == UsbdPipeTypeBulk) {
                if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                    inPipe = pipeInfo.PipeId;
                } else {
                    outPipe = pipeInfo.PipeId;
                }
            }
        }
        if (inPipe == 0 || outPipe == 0) {
            BulkTrace("skip iface %u: no bulk pair (in=0x%02X out=0x%02X, %u endpoints)",
                      (unsigned)ifaceDesc.bInterfaceNumber,
                      (unsigned)inPipe, (unsigned)outPipe,
                      (unsigned)ifaceDesc.bNumEndpoints);
            WinUsb_Free(tempWinUSB);
            CloseHandle(hDevice);
            free(deviceDetailData);
            continue;
        }

        // ---- 命中 ----
        g_winusb.deviceHandle = hDevice;
        g_winusb.winusbHandle = tempWinUSB;
        g_winusb.bulkInPipe   = inPipe;
        g_winusb.bulkOutPipe  = outPipe;
        deviceOpened = true;

        // 读接口自己的产品名（"CMSIS-DAP v2"），供 CMSIS_DAP_Identify 使用
        _readInterfaceString(tempWinUSB, ifaceDesc.iInterface);

        BulkTrace("opened V2 iface %u: bulkIn=0x%02X bulkOut=0x%02X path='%s'",
                  (unsigned)ifaceDesc.bInterfaceNumber,
                  (unsigned)inPipe, (unsigned)outPipe, deviceDetailData->DevicePath);

        free(deviceDetailData);
        break;
    }

    SetupDiDestroyDeviceInfoList(deviceInfoSet);

    if (candidatesOut) {
        *candidatesOut += candidates;
    }
    return deviceOpened;
}

/**
 * @brief 查找并打开指定 VID/PID 的 USB 设备
 *
 * 依次尝试 CMSIS-DAP v2 专用 GUID 与普通 USB 设备 GUID：
 * 前者才能拿到 MI_05 这类 WinUSB 接口节点，后者仅作兜底。
 * 序列号筛选尚未实现（见 COMPAT_ANALYSIS.md）。
 */
static bool _findAndOpenDevice(uint16_t vid, uint16_t pid, const char* serial)
{
    (void)serial;

    // 依次尝试三种接口 GUID，并逐个记录枚举到几个候选 —— 出问题时能直接看出
    // 是哪一类 GUID 枚举不到东西，而不是只看到一个总数。
    static const GUID* const guids[] = {
        &WINUSB_GUID_DEVINTERFACE,        // {DEE824EF-...} 标准 WinUSB 接口（优先）
        &CMSIS_DAP_V2_GUID_DEVINTERFACE,  // {CDB3B5AD-...} CMSIS-DAP v2 专用
        &USB_GUID_DEVINTERFACE,           // {A5DCBF10-...} 普通 USB 设备（兜底）
    };
    static const char* const names[] = { "WinUSB", "CMSIS-DAPv2", "USBDevice" };

    int  candidates = 0;
    bool opened = false;

    for (int i = 0; i < 3 && !opened; ++i) {
        const int before = candidates;
        if (_findAndOpenDeviceInGuid(guids[i], vid, pid, &candidates)) {
            opened = true;
        }
        BulkTrace("guid %s: %d candidate(s)", names[i], candidates - before);
    }

    // 汇总行：candidates=0 说明三种 GUID 下都没有 vid/pid 匹配的接口路径，
    // 问题在枚举（设备未插 / 驱动未装），而不是在打开或驱动绑定。
    BulkTrace("enumeration done: %d candidate(s) for vid_%04X&pid_%04X, opened=%d",
              candidates, vid, pid, (int)opened);
    return opened;
}

/**
 * @brief 初始化 WinUSB 接口
 */
static bool _initWinUSBInterface(void)
{
    if (!g_winusb.winusbHandle) {
        return false;
    }

    // 设置 Bulk 端点超时 - WinUSB 不需要获取描述符
    // 端点已在 _findAndOpenDevice 中验证
    (void)g_winusb;  // 避免未使用警告
    return true;
}

/**
 * @brief 关闭 WinUSB 设备
 */
static void _closeWinUSB(void)
{
    if (g_winusb.winusbHandle) {
        WinUsb_Free(g_winusb.winusbHandle);
        g_winusb.winusbHandle = NULL;
    }

    if (g_winusb.deviceHandle && g_winusb.deviceHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_winusb.deviceHandle);
        g_winusb.deviceHandle = INVALID_HANDLE_VALUE;
    }

    memset(&g_winusb, 0, sizeof(g_winusb));
    g_winusb.deviceHandle = INVALID_HANDLE_VALUE;
}

/**
 * @brief WinUSB Bulk 写入
 */
static int _bulkWrite(const uint8_t* data, size_t len, int timeoutMs)
{
    if (!g_winusb.winusbHandle || !data || len == 0) {
        return -1;
    }

    ULONG bytesWritten = 0;
    BOOL result;

    if (timeoutMs <= 0) {
        timeoutMs = DEFAULT_BULK_TIMEOUT;
    }

    // 创建覆盖结构用于超时
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return -1;
    }

    // 设置超时
    result = WinUsb_WritePipe(g_winusb.winusbHandle,
                              g_winusb.bulkOutPipe,
                              (PUCHAR)data,
                              (ULONG)len,
                              &bytesWritten,
                              &overlapped);

    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // 等待操作完成
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (waitResult == WAIT_TIMEOUT) {
                CancelIo(g_winusb.deviceHandle);
                CloseHandle(overlapped.hEvent);
                return -2;  // 超时
            } else if (waitResult == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(g_winusb.deviceHandle, &overlapped, &bytesWritten, FALSE)) {
                    CloseHandle(overlapped.hEvent);
                    return -1;
                }
            }
        } else {
            CloseHandle(overlapped.hEvent);
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (int)bytesWritten;
}

/**
 * @brief WinUSB Bulk 读取
 */
static int _bulkRead(uint8_t* data, size_t maxLen, int timeoutMs)
{
    if (!g_winusb.winusbHandle || !data || maxLen == 0) {
        return -1;
    }

    ULONG bytesRead = 0;
    BOOL result;

    if (timeoutMs <= 0) {
        timeoutMs = DEFAULT_BULK_TIMEOUT;
    }

    // 创建覆盖结构用于超时
    OVERLAPPED overlapped = {0};
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) {
        return -1;
    }

    // 设置超时
    result = WinUsb_ReadPipe(g_winusb.winusbHandle,
                             g_winusb.bulkInPipe,
                             data,
                             (ULONG)maxLen,
                             &bytesRead,
                             &overlapped);

    if (!result) {
        DWORD error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            // 等待操作完成
            DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
            if (waitResult == WAIT_TIMEOUT) {
                CancelIo(g_winusb.deviceHandle);
                CloseHandle(overlapped.hEvent);
                return -2;  // 超时
            } else if (waitResult == WAIT_OBJECT_0) {
                if (!GetOverlappedResult(g_winusb.deviceHandle, &overlapped, &bytesRead, FALSE)) {
                    CloseHandle(overlapped.hEvent);
                    return -1;
                }
            }
        } else {
            CloseHandle(overlapped.hEvent);
            return -1;
        }
    }

    CloseHandle(overlapped.hEvent);
    return (int)bytesRead;
}

/**
 * @brief 初始化 WinUSB (V2 Bulk 模式)
 */
static bool _initWinUSB(uint16_t vid, uint16_t pid, const char* serial)
{
    // 初始化上下文
    memset(&g_winusb, 0, sizeof(g_winusb));
    g_winusb.deviceHandle = INVALID_HANDLE_VALUE;

    // 查找并打开设备
    if (!_findAndOpenDevice(vid, pid, serial)) {
        return false;
    }

    // 初始化 WinUSB 接口
    if (!_initWinUSBInterface()) {
        _closeWinUSB();
        return false;
    }

    g_winusb.initialized = true;
    return true;
}

// ============================================================================
// 公共 API
// ============================================================================

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Init(uint16_t vid, uint16_t pid, const char* serial)
{
    // 这一行只在**当前构建**里存在。日志里若完全没有它，说明 Keil 加载的
    // 仍是旧 DLL（已部署但 µVision 没重启），不要据此判断设备问题。
    BulkTrace("Init: entered (vid=0x%04X pid=0x%04X serial='%s')",
              vid ? vid : ORBTRACE_VID, pid ? pid : ORBTRACE_PID,
              serial ? serial : "");

    // 已经通过 V2 打开、且句柄仍然有效时直接复用，不要拆了重开。
    //
    // 原因：AGDI 在一次调试会话里会反复调用 rddi_Open（实测 7 次）。每次
    // "先 Shutdown 再重开"都要重新 CreateFile，而 WinUSB 设备是**独占**的 ——
    // 上一个句柄尚未完全释放时第二次 CreateFile 会得到
    // ERROR_ACCESS_DENIED(5)，随后所有打开都退化成 V1 HID。
    // 实测现象：7 次 RDDI_Open 里只有第 1 次是 V2，其余 6 次全变 V1。
    if (g_ctx.initialized && g_ctx.mode == USB_BULK_BULK_MODE && g_winusb.winusbHandle) {
        BulkTrace("Init: reuse existing V2 connection (skip reopen)");
        return 0;
    }

    if (g_ctx.initialized) {
        ORBMDK_USB_Bulk_Shutdown();
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.timeoutMs = 1000;
    g_ctx.timerFrequency = 10000000;  // 10 MHz 默认值

    // 尝试初始化 V2 Bulk 模式
    if (_initWinUSB(vid ? vid : ORBTRACE_VID, pid ? pid : ORBTRACE_PID, serial)) {
        g_ctx.mode = USB_BULK_BULK_MODE;
        g_ctx.deviceInfo.protocol_version = 2;
        g_ctx.deviceInfo.max_packet_size = 512;  // USB Full Speed Bulk
        g_ctx.deviceInfo.max_packet_count = 1;
        g_ctx.initialized = true;
        BulkTrace("Init: V2 Bulk mode OK (product='%s')", g_bulkProductName);
        return 0;
    }

    BulkTrace("Init: V2 Bulk unavailable -> fall back to V1 HID");

    // Fallback 到 HID 模式
    int ret = ORBMDK_HID_Init();
    if (ret != 0) {
        BulkTrace("Init: HID_Init failed (%d)", ret);
        return -1;
    }

    ret = ORBMDK_HID_OpenDevice(serial);
    if (ret != 0) {
        BulkTrace("Init: HID_OpenDevice failed (%d)", ret);
        ORBMDK_HID_Shutdown();
        return -1;
    }

    g_ctx.initialized = true;
    g_ctx.mode = USB_BULK_HID_MODE;
    g_ctx.hidAvailable = true;
    g_ctx.deviceInfo.vid = vid ? vid : ORBTRACE_VID;
    g_ctx.deviceInfo.pid = pid ? pid : ORBTRACE_PID;
    g_ctx.deviceInfo.protocol_version = 1;
    g_ctx.deviceInfo.max_packet_size = 64;

    BulkTrace("Init: V1 HID mode OK");
    return 0;
}

ORBMDK_INTERNAL void ORBMDK_USB_Bulk_Shutdown(void)
{
    if (!g_ctx.initialized) return;

    if (g_ctx.mode == USB_BULK_BULK_MODE) {
        _closeWinUSB();
    }

    if (g_ctx.hidAvailable) {
        ORBMDK_HID_CloseDevice();
        ORBMDK_HID_Shutdown();
        g_ctx.hidAvailable = false;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.initialized = false;
}

ORBMDK_INTERNAL USB_Bulk_Mode ORBMDK_USB_Bulk_GetMode(void)
{
    return g_ctx.initialized ? g_ctx.mode : USB_BULK_NOT_INITED;
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetDeviceInfo(USB_Bulk_Device_Info* info)
{
    if (!g_ctx.initialized || !info) return -1;
    memcpy(info, &g_ctx.deviceInfo, sizeof(USB_Bulk_Device_Info));
    return 0;
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_DAPCommand(const uint8_t* cmd, size_t cmdLen,
                                          uint8_t* resp, size_t* respLen, int timeoutMs)
{
    if (!g_ctx.initialized) return -1;

    if (g_ctx.mode == USB_BULK_BULK_MODE) {
        // CMSIS-DAP v2 规定：Bulk 通道上的命令包必须**补齐到 wMaxPacketSize**
        // （本设备 512 字节），设备按整包读取；不足一包时它不会响应。
        //
        // 之前这里直接把 cmdLen 字节写下去，结果是通道能打开、但任何 DAP 命令
        // 都得不到响应 —— 表现为 DAP_ConnectTarget failed / Keil 报
        // "rddi-dap error"，随后 AGDI 退回 V1。
        uint8_t pkt[512];
        if (cmdLen > sizeof(pkt)) {
            return -1;
        }
        memset(pkt, 0, sizeof(pkt));
        memcpy(pkt, cmd, cmdLen);

        int ret = _bulkWrite(pkt, sizeof(pkt), timeoutMs);
        if (ret < 0) return ret;

        // 响应同样是整包，统一按 512 读入后按调用方缓冲大小截断
        uint8_t rbuf[512];
        int n = _bulkRead(rbuf, sizeof(rbuf), timeoutMs);
        if (n < 0) return n;

        const size_t copy = ((size_t)n < *respLen) ? (size_t)n : *respLen;
        memcpy(resp, rbuf, copy);
        *respLen = copy;
        return (int)copy;
    } else {
        // V1 HID 模式
        return ORBMDK_HID_DAPCommand(cmd, cmdLen, resp, respLen, timeoutMs);
    }
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Write(const uint8_t* data, size_t len, int timeoutMs)
{
    if (!g_ctx.initialized) return -1;

    if (g_ctx.mode == USB_BULK_BULK_MODE) {
        return _bulkWrite(data, len, timeoutMs);
    }

    // HID 模式不支持直接 Bulk 写入
    return -1;
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Read(uint8_t* data, size_t maxLen, int timeoutMs)
{
    if (!g_ctx.initialized) return -1;

    if (g_ctx.mode == USB_BULK_BULK_MODE) {
        return _bulkRead(data, maxLen, timeoutMs);
    }

    // HID 模式不支持直接 Bulk 读取
    return -1;
}

ORBMDK_INTERNAL uint32_t ORBMDK_USB_Bulk_GetCapabilities(void)
{
    if (!g_ctx.initialized) return 0;

    // 返回 V2 能力标志
    uint32_t caps = DAPV2_CAP_SWD | DAPV2_CAP_JTAG | DAPV2_CAP_ATOMIC;
    if (g_ctx.mode == USB_BULK_BULK_MODE) {
        caps |= DAPV2_CAP_SWO_UART | DAPV2_CAP_SWO_MANCHESTER | DAPV2_CAP_SWO_STREAMING |
                DAPV2_CAP_TPIU | DAPV2_CAP_ITM | DAPV2_CAP_PC_SAMPLE;
    }
    return caps;
}

ORBMDK_INTERNAL void ORBMDK_USB_Bulk_SetTimeout(int timeoutMs)
{
    g_ctx.timeoutMs = timeoutMs;
}

// ============================================================================
// V2 协议 API
// ============================================================================

ORBMDK_INTERNAL int CMSIS_DAP_V2_GetInfo(uint8_t info_id, uint8_t* buffer, size_t buffer_len)
{
    if (!g_ctx.initialized) return -1;

    if (!buffer || buffer_len == 0) return -1;

    switch (info_id) {
    case DAPV2_INFO_PRODUCT_NAME:
        strncpy_s((char*)buffer, buffer_len, "ORBTrace CMSIS-DAP", buffer_len - 1);
        return (int)strlen((char*)buffer);

    case DAPV2_INFO_SERIAL_NUMBER:
        buffer[0] = '\0';
        return 1;

    case DAPV2_INFO_FIRMWARE_VERSION:
        strncpy_s((char*)buffer, buffer_len, "1.0.0", buffer_len - 1);
        return (int)strlen((char*)buffer);

    case DAPV2_INFO_VENDOR_STRING:
        strncpy_s((char*)buffer, buffer_len, "ORBTrace", buffer_len - 1);
        return (int)strlen((char*)buffer);

    case DAPV2_INFO_CAPABILITIES:
        buffer[0] = (uint8_t)(ORBMDK_USB_Bulk_GetCapabilities() & 0xFF);
        return 1;

    case DAPV2_INFO_TEST_DOMAIN_TIMER:
        // 返回 Timer 频率
        if (buffer_len >= 4) {
            uint32_t freq = g_ctx.timerFrequency;
            buffer[0] = (uint8_t)(freq & 0xFF);
            buffer[1] = (uint8_t)((freq >> 8) & 0xFF);
            buffer[2] = (uint8_t)((freq >> 16) & 0xFF);
            buffer[3] = (uint8_t)((freq >> 24) & 0xFF);
            return 4;
        }
        return -1;

    case DAPV2_INFO_CAPABILITIES_1:
        if (buffer_len >= 4) {
            uint32_t caps = ORBMDK_USB_Bulk_GetCapabilities();
            buffer[0] = (uint8_t)(caps & 0xFF);
            buffer[1] = (uint8_t)((caps >> 8) & 0xFF);
            buffer[2] = (uint8_t)((caps >> 16) & 0xFF);
            buffer[3] = (uint8_t)((caps >> 24) & 0xFF);
            return 4;
        }
        return -1;

    default:
        return -1;
    }
}

ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStart(void)
{
    if (!g_ctx.initialized) return -1;

    g_ctx.timerStarted = true;
    g_ctx.timerStart = std::chrono::steady_clock::now();
    return 0;
}

ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerRead(uint32_t* timestamp)
{
    if (!g_ctx.initialized || !timestamp) return -1;

    if (!g_ctx.timerStarted) {
        *timestamp = 0;
        return -1;
    }

    // 计算经过的时钟周期
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - g_ctx.timerStart);
    *timestamp = (uint32_t)((elapsed.count() * g_ctx.timerFrequency) / 1000000);
    return 0;
}

ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStop(void)
{
    if (!g_ctx.initialized) return -1;

    g_ctx.timerStarted = false;
    return 0;
}

ORBMDK_INTERNAL int CMSIS_DAP_V2_SetTimeout(int timeout_ms)
{
    if (!g_ctx.initialized) return -1;

    g_ctx.timeoutMs = timeout_ms;
    return 0;
}

ORBMDK_INTERNAL uint8_t CMSIS_DAP_V2_GetProtocolVersion(void)
{
    return g_ctx.initialized ? g_ctx.deviceInfo.protocol_version : 0;
}

ORBMDK_INTERNAL uint32_t CMSIS_DAP_V2_GetCapabilities(void)
{
    if (!g_ctx.initialized) return 0;
    return ORBMDK_USB_Bulk_GetCapabilities();
}

// ============================================================================
// Bulk 设备直接访问
// ============================================================================

ORBMDK_INTERNAL int ORBMDK_Bulk_Open(uint16_t vid, uint16_t pid, const char* serial)
{
    return ORBMDK_USB_Bulk_Init(vid, pid, serial);
}

ORBMDK_INTERNAL void ORBMDK_Bulk_Close(void)
{
    ORBMDK_USB_Bulk_Shutdown();
}

ORBMDK_INTERNAL int ORBMDK_Bulk_Write(const uint8_t* data, size_t len, int timeout_ms)
{
    return ORBMDK_USB_Bulk_Write(data, len, timeout_ms);
}

ORBMDK_INTERNAL int ORBMDK_Bulk_Read(uint8_t* data, size_t max_len, int timeout_ms)
{
    return ORBMDK_USB_Bulk_Read(data, max_len, timeout_ms);
}
