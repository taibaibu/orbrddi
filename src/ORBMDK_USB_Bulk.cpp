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
#include "ORBMDK_Log.h"

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
    // ⚠️ 必须是 USHORT：wMaxPacketSize 是 16 位，orbtrace 声明的就是 **512**。
    // 历史 bug：这里（以及 WinUsb_QueryPipe 的取值）被存成/转成 UCHAR，
    // 512 & 0xFF == 0，于是包长"变成 0"，后续一连串判断全被带偏。
    USHORT bulkOutMaxPkt;         // OUT 端点 wMaxPacketSize（来自配置描述符）
    USHORT bulkInMaxPkt;          // IN 端点 wMaxPacketSize
    UCHAR  deviceSpeed;           // 1=Low 2=Full 3=High（WinUsb DEVICE_SPEED）
    USHORT alignedOutPkt;         // 自标定后的实际出包长度（0 = 未标定）
    bool initialized;
};

static WinUSBContext g_winusb = {};

// 定义在文件后部（用到 g_winusb / deviceSpeed），此处先声明供枚举阶段调用
static size_t _bulkPacketSize(size_t cmdLen);

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
// 诊断日志（统一实现见 src/ORBMDK_Log.cpp，COMPAT_ANALYSIS §8.3）
//
// 本文件此前完全没有日志（见 §8.1），V2 打不开时无从排查；后来补了 BulkTrace，
// 但它绕过级别、绕过 ORBMDK_LOG_FILE、且手写前缀（§8.2(2)(3)）。
// 现在走统一入口：**仍然不受级别控制、仍然只落盘**，但会遵循 ORBMDK_LOG_FILE，
// 并带上时间戳与进程/线程号。
// 只记关键路径（枚举到的接口、被拒绝的原因、最终选中的端点），不记逐次传输。
// ---------------------------------------------------------------------------
#define ORBMDK_LOG_MODULE "BULK"
#define BulkTrace(...) ORBMDK_LOG_TRACE(__VA_ARGS__)

// V2 接口的产品名（来自接口字符串描述符，通常是 "CMSIS-DAP v2"）
static char g_bulkProductName[128] = {0};

// V2 接口所属设备的序列号（来自 iSerialNumber 字符串描述符）
static char g_bulkSerial[128] = {0};

// ---------------------------------------------------------------------------
// 传输模式选择
//
// V1(HID) 与 V2(Bulk) **两种都要能显式选择使用**，而选择权**唯一**来自用户在
// µVision 对话框里点的那一条适配器（AGDI 把条目序号透传到
// CMSIS_DAP_ConfigureInterface）。没有隐藏开关、没有文件/环境变量、也不允许
// "这条打不开就偷偷换另一条顶上"：
//   ifNo 0 -> CMSIS-DAP v2 (USB Bulk)：只用 V2，打不开就如实失败
//   ifNo 1 -> CMSIS-DAP v1 (HID)      ：只用 V1
//   尚未选定（RDDI_Open 早于 ConfigureInterface）-> AUTO：V2 优先，不可用才 V1
// ---------------------------------------------------------------------------
static const char* _transportName(int pref)
{
    return pref == USB_BULK_TRANSPORT_BULK ? "bulk"
         : (pref == USB_BULK_TRANSPORT_HID ? "hid" : "auto");
}

// ---------------------------------------------------------------------------
// 显式指定传输（来自 AGDI 的 CMSIS_DAP_ConfigureInterface(ifNo)）
//
// AGDI 的顺序是：RDDI_Open → CMSIS_DAP_Detect → Identify(逐接口) →
// CMSIS_DAP_ConfigureInterface(handle, ifNo, cfg) → DAP_Configure → Connect。
// 也就是说"用户选了哪个接口"是在 ConfigureInterface 才告诉我们的，
// 此时 RDDI_Open 早已按缺省偏好打开了一个传输层 —— 所以必须能在这里**换过去**。
//
// 这是传输层**唯一**的选择来源：用户在对话框里的选择就是最终结果。
// -1 表示尚未指定（RDDI_Open 的窗口期，此时按 AUTO 打开一条可用的通道）。
// ---------------------------------------------------------------------------
static int g_forcedPref = -1;

static int _transportPreference(void)
{
    // 传输层**唯一**由 AGDI 在 CMSIS_DAP_ConfigureInterface(ifNo) 里选中的那一条
    // 适配器决定（ifNo 0 = CMSIS-DAP v2 / USB Bulk，ifNo 1 = CMSIS-DAP v1 / HID）。
    //
    // 这里刻意**不再**支持 %TEMP%\ORBMDK_TRANSPORT 文件与环境变量：那种"隐藏开关"
    // 会让人把"V2 打不开"当成"切个开关就好了"，从而把真正的缺陷（句柄泄漏导致
    // err=5）藏起来 —— 历史上就是这么被掩盖了很久。选谁就用谁，打不开就报错。
    return (g_forcedPref >= 0) ? g_forcedPref : USB_BULK_TRANSPORT_AUTO;
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetTransportPreference(void)
{
    return _transportPreference();
}

// ---------------------------------------------------------------------------
// 多调试器：设备选择
//
// 同机插多台 CMSIS-DAP 时，AGDI（Keil）在 rddi_Open 传 pDetails = NULL，
// **不提供**"用哪一台"的信息 —— 实测确认，所以本层只能按枚举顺序取第一台。
// 刻意**不**提供"指定序列号"的隐藏开关（与传输层同一条原则：不留任何能
// 掩盖问题的旁路）。序列号仍然会被读出来用于 Identify(idNo=3)，只是不参与筛选。
// ---------------------------------------------------------------------------

/**
 * @brief 按"接口序号"选定传输层
 *
 * 序号映射与 CMSIS_DAP_Identify 里的产品名一一对应，保持稳定（用户的
 * 选择会保存在工程里，跨会话复用）：
 *   ifNo 0 -> CMSIS-DAP v2 (USB Bulk)
 *   ifNo 1 -> CMSIS-DAP v1 (HID)
 *
 * 当前已经是目标模式时直接返回（不做无谓的关闭/重开）；否则调用
 * ORBMDK_USB_Bulk_Init —— 它内部已有"模式切换"分支。
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_SelectInterface(int ifNo)
{
    const int pref = (ifNo == 1) ? USB_BULK_TRANSPORT_HID : USB_BULK_TRANSPORT_BULK;
    g_forcedPref = pref;

    const USB_Bulk_Mode cur = ORBMDK_USB_Bulk_GetMode();
    const bool already = (pref == USB_BULK_TRANSPORT_BULK && cur == USB_BULK_BULK_MODE) ||
                         (pref == USB_BULK_TRANSPORT_HID  && cur == USB_BULK_HID_MODE);
    BulkTrace("SelectInterface: ifNo=%d -> %s (current=%d, already=%d)",
              ifNo, _transportName(pref), (int)cur, (int)already);

    if (already) {
        return 0;
    }
    return ORBMDK_USB_Bulk_Init(0, 0, nullptr);
}

/**
 * @brief 某个"接口序号"对应的适配器名字（AGDI 对话框列表里显示的就是它）
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetInterfaceName(int ifNo, char* buf, size_t len)
{
    if (!buf || len == 0) {
        return -1;
    }
    buf[0] = '\0';

    if (ifNo == 1) {
        // V1：HID 接口自己的字符串描述符就是 "CMSIS-DAP v1"
        strncpy_s(buf, len, "CMSIS-DAP v1", _TRUNCATE);
        return 0;
    }

    if (g_bulkProductName[0] != '\0') {
        strncpy_s(buf, len, g_bulkProductName, _TRUNCATE);
    } else {
        strncpy_s(buf, len, "CMSIS-DAP v2", _TRUNCATE);
    }
    return 0;
}

/**
 * @brief 读取设备序列号（iSerialNumber 字符串描述符）
 *
 * V2 模式下没有打开 HID 接口，HidD_GetSerialNumberString 用不了，于是
 * CMSIS_DAP_Identify(idNo=3) 只能返回 "Unknown"，µVision 适配器列表里
 * 看不到序列号。这里直接从 WinUSB 句柄读。
 */
static void _readDeviceSerial(WINUSB_INTERFACE_HANDLE h)
{
    g_bulkSerial[0] = '\0';

    USB_DEVICE_DESCRIPTOR desc;
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(h, USB_DEVICE_DESCRIPTOR_TYPE, 0, 0x0409,
                              reinterpret_cast<PUCHAR>(&desc), sizeof(desc), &got)) {
        BulkTrace("device descriptor unavailable (err=%lu)", (unsigned long)GetLastError());
        return;
    }
    if (desc.iSerialNumber == 0) {
        BulkTrace("device has no serial number string");
        return;
    }

    UCHAR buf[256] = {0};
    got = 0;
    if (!WinUsb_GetDescriptor(h, USB_STRING_DESCRIPTOR_TYPE, desc.iSerialNumber, 0x0409,
                              buf, sizeof(buf), &got) || got < 2) {
        BulkTrace("serial string %u unavailable (got=%lu)",
                  (unsigned)desc.iSerialNumber, (unsigned long)got);
        return;
    }

    const int chars = (int)((got - 2) / 2);
    const int n = WideCharToMultiByte(CP_ACP, 0, reinterpret_cast<LPCWSTR>(buf + 2), chars,
                                      g_bulkSerial, (int)sizeof(g_bulkSerial) - 1,
                                      nullptr, nullptr);
    g_bulkSerial[(n > 0) ? n : 0] = '\0';
    BulkTrace("device serial = '%s'", g_bulkSerial);
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetSerialNumber(char* buf, size_t len)
{
    if (!buf || len == 0) {
        return -1;
    }
    buf[0] = '\0';
    if (g_bulkSerial[0] == '\0') {
        return -1;
    }
    strncpy_s(buf, len, g_bulkSerial, _TRUNCATE);
    return 0;
}

/**
 * @brief 从**配置描述符**里取该接口的 Bulk 端点包长
 *
 * 为什么不只用 WinUsb_QueryPipe：orbtrace 的 MI_05 通过 QueryPipe 拿到的
 * `MaximumPacketSize` 是 **0**（实测），据此推断包长不可靠。
 * 端点包长以**描述符声明**为准 —— 这才是设备的真实能力，
 * 也是出包长度该用的值（见 §13.2(2)：包长错了 V2 直接不通）。
 *
 * @param h            WinUSB 接口句柄
 * @param ifaceNumber  目标接口号
 * @param inMax/outMax 输出：Bulk IN / OUT 的 wMaxPacketSize（未找到为 0）
 * @return true 表示解析成功
 */
static bool _readEndpointPacketSizes(WINUSB_INTERFACE_HANDLE h, UCHAR ifaceNumber,
                                     USHORT* inMax, USHORT* outMax)
{
    if (inMax)  *inMax = 0;
    if (outMax) *outMax = 0;
    if (!h) {
        return false;
    }

    // wTotalLength 最大 65535，实际复合设备通常几百字节；4096 足够
    uint8_t buf[4096] = {0};
    ULONG got = 0;
    if (!WinUsb_GetDescriptor(h, USB_CONFIGURATION_DESCRIPTOR_TYPE, 0, 0x0409,
                              buf, sizeof(buf), &got) || got < sizeof(USB_CONFIGURATION_DESCRIPTOR)) {
        BulkTrace("config descriptor unavailable (got=%lu, err=%lu)",
                  (unsigned long)got, (unsigned long)GetLastError());
        return false;
    }

    const auto* cfg = reinterpret_cast<const USB_CONFIGURATION_DESCRIPTOR*>(buf);
    ULONG total = cfg->wTotalLength;
    if (total > got) {
        total = got;                         // 以实际读到的为准，避免越界
    }

    UCHAR curIface = 0xFF;
    bool  inIface = false;
    for (ULONG off = 0; off + 2 <= total; ) {
        const uint8_t len = buf[off];
        const uint8_t type = buf[off + 1];
        if (len < 2 || off + len > total) {
            break;                            // 描述符链坏了，停止解析
        }

        if (type == USB_INTERFACE_DESCRIPTOR_TYPE && len >= sizeof(USB_INTERFACE_DESCRIPTOR)) {
            const auto* itf = reinterpret_cast<const USB_INTERFACE_DESCRIPTOR*>(buf + off);
            curIface = itf->bInterfaceNumber;
            inIface = (curIface == ifaceNumber);
            BulkTrace("  desc iface %u alt=%u class=0x%02X eps=%u%s",
                      (unsigned)curIface, (unsigned)itf->bAlternateSetting,
                      (unsigned)itf->bInterfaceClass, (unsigned)itf->bNumEndpoints,
                      inIface ? "  <- target" : "");
        } else if (type == USB_ENDPOINT_DESCRIPTOR_TYPE &&
                   len >= sizeof(USB_ENDPOINT_DESCRIPTOR)) {
            const auto* ep = reinterpret_cast<const USB_ENDPOINT_DESCRIPTOR*>(buf + off);
            const bool isBulk = ((ep->bmAttributes & 0x03) == 0x02);
            BulkTrace("  desc   ep 0x%02X attr=0x%02X type=%s wMaxPacketSize=%u",
                      (unsigned)ep->bEndpointAddress, (unsigned)ep->bmAttributes,
                      isBulk ? "Bulk" : "other",
                      (unsigned)ep->wMaxPacketSize);
            if (inIface && isBulk) {
                if (USB_ENDPOINT_DIRECTION_IN(ep->bEndpointAddress)) {
                    if (inMax)  *inMax  = ep->wMaxPacketSize;
                } else {
                    if (outMax) *outMax = ep->wMaxPacketSize;
                }
            }
        }
        off += len;
    }

    BulkTrace("iface %u endpoint wMaxPacketSize (from config descriptor): in=%u out=%u",
              (unsigned)ifaceNumber, (unsigned)(inMax ? *inMax : 0),
              (unsigned)(outMax ? *outMax : 0));
    return ((inMax && *inMax) || (outMax && *outMax)) ? true : false;
}

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
 * @param serial        指定序列号（可选，空表示不筛选）
 * @param candidatesOut 累计已通过 VID/PID 路径过滤的候选接口数
 */
static bool _findAndOpenDeviceInGuid(const GUID* guid, uint16_t vid, uint16_t pid,
                                     const char* serial, int* candidatesOut)
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
        USHORT inMaxPkt = 0, outMaxPkt = 0;
        for (UCHAR i = 0; i < ifaceDesc.bNumEndpoints; i++) {
            WINUSB_PIPE_INFORMATION pipeInfo;
            if (WinUsb_QueryPipe(tempWinUSB, 0, i, &pipeInfo) &&
                pipeInfo.PipeType == UsbdPipeTypeBulk) {
                if (USB_ENDPOINT_DIRECTION_IN(pipeInfo.PipeId)) {
                    inPipe = pipeInfo.PipeId;
                    inMaxPkt = pipeInfo.MaximumPacketSize;   // USHORT，勿截断
                } else {
                    outPipe = pipeInfo.PipeId;
                    outMaxPkt = pipeInfo.MaximumPacketSize;
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

        // ---- 序列号筛选（多调试器场景）----
        // 设备路径里是**位置型**实例 ID（...&mi_05#6&ad6fa01&0&0005#...），
        // 拿不到序列号，只能打开后读 iSerialNumber 描述符来比对。
        // 未指定序列号时不筛选，保持原来的"第一台"行为。
        if (serial && serial[0] != '\0') {
            _readDeviceSerial(tempWinUSB);   // 填充 g_bulkSerial
            if (strcmp(g_bulkSerial, serial) != 0) {
                BulkTrace("  skip iface %u: serial '%s' != requested '%s'",
                          (unsigned)ifaceDesc.bInterfaceNumber, g_bulkSerial, serial);
                WinUsb_Free(tempWinUSB);
                CloseHandle(hDevice);
                free(deviceDetailData);
                continue;
            }
            BulkTrace("  serial match: '%s'", g_bulkSerial);
        }

        // ---- 命中 ----
        g_winusb.deviceHandle = hDevice;
        g_winusb.winusbHandle = tempWinUSB;
        g_winusb.bulkInPipe   = inPipe;
        g_winusb.bulkOutPipe  = outPipe;
        g_winusb.bulkInMaxPkt = inMaxPkt;
        g_winusb.bulkOutMaxPkt = outMaxPkt;
        deviceOpened = true;

        // 端点包长**以描述符为准**
        USHORT descInMax = 0, descOutMax = 0;
        if (_readEndpointPacketSizes(tempWinUSB, ifaceDesc.bInterfaceNumber,
                                     &descInMax, &descOutMax)) {
            if (descInMax)  g_winusb.bulkInMaxPkt  = descInMax;
            if (descOutMax) g_winusb.bulkOutMaxPkt = descOutMax;
        }

        // 端口速度：bulk 单包上限由它决定（FS=64 / HS=512），见 _bulkPacketSize
        {
            ULONG speed = 0, cbSpeed = sizeof(speed);
            if (WinUsb_QueryDeviceInformation(tempWinUSB, DEVICE_SPEED, &cbSpeed, &speed)) {
                g_winusb.deviceSpeed = (UCHAR)speed;
            }
        }

        // 读接口自己的产品名（"CMSIS-DAP v2"）与设备序列号，供 CMSIS_DAP_Identify 使用
        _readInterfaceString(tempWinUSB, ifaceDesc.iInterface);
        _readDeviceSerial(tempWinUSB);

        BulkTrace("opened V2 iface %u: bulkIn=0x%02X(pkt=%u) bulkOut=0x%02X(pkt=%u) "
                  "speed=%u -> effective outPacket=%u path='%s'",
                  (unsigned)ifaceDesc.bInterfaceNumber,
                  (unsigned)inPipe, (unsigned)g_winusb.bulkInMaxPkt,
                  (unsigned)outPipe, (unsigned)g_winusb.bulkOutMaxPkt,
                  (unsigned)g_winusb.deviceSpeed,
                  (unsigned)_bulkPacketSize(1),
                  deviceDetailData->DevicePath);

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
        if (_findAndOpenDeviceInGuid(guids[i], vid, pid, serial, &candidates)) {
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
        // ★ 释放顺序是**硬性要求**（现场实测 + 内核语义）：
        //     1) CancelIoEx(句柄, NULL)     取消本进程在该句柄上**所有**挂起的 IRP
        //     2) WinUsb_AbortPipe(IN/OUT)   同步等待这些 IRP 真正结束（ResetPipe 复位状态）
        //     3) WinUsb_Free + CloseHandle  此时才允许释放
        //
        // 少做 1)/2) 的后果是**永久性**的：内核仍持有 file object 引用 →
        // 该 WinUSB 接口不会被释放 → 此后**任何进程**（含 Keil 自己、含命令行）
        // 对它 CreateFile 都返回 ERROR_ACCESS_DENIED(5)。而 WinUSB 独占，
        // 于是 V2 通道再也打不开，只能靠"完全退出 µVision 进程"恢复。
        //
        // 实测证据（本机）：V2 会话结束后，同一 UV4 进程里后续 7 次 RDDI_Open
        // 全部 `reject: CreateFile failed, err=5`；同时用独立进程探测同一设备
        // 的其它 WinUSB 接口（mi_02/03/06/07）都能打开，唯独 mi_05 是 err=5
        // —— 即独占者就是本进程里这段没释放干净的句柄。
        if (g_winusb.deviceHandle && g_winusb.deviceHandle != INVALID_HANDLE_VALUE) {
            if (!CancelIoEx(g_winusb.deviceHandle, NULL)) {
                const DWORD e = GetLastError();
                if (e != ERROR_NOT_FOUND) {   // 没有挂起 I/O 属正常
                    BulkTrace("close: CancelIoEx err=%lu", (unsigned long)e);
                }
            }
        }

        if (g_winusb.bulkInPipe) {
            WinUsb_AbortPipe(g_winusb.winusbHandle, g_winusb.bulkInPipe);
            WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkInPipe);
        }
        if (g_winusb.bulkOutPipe) {
            WinUsb_AbortPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);
            WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);
        }

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
 * @brief 取消一个未完成的 OVERLAPPED 操作，并**等它真正结束**再返回
 *
 * 关键：CancelIoEx 是**异步**的，返回时那个 IRP 很可能还挂在驱动里。
 * 此时若直接 CloseHandle(事件) / WinUsb_Free + CloseHandle(设备)，
 * 内核会一直持有 file object 的引用 → **设备接口不会被释放** →
 * 之后任何 CreateFile 都返回 ERROR_ACCESS_DENIED(5)，而且是**永久**的。
 *
 * 实测现象：一次 bulk 超时之后，V2 通道整场会话都再打不开，
 * 剩下全部退化成 V1 HID（日志里 `reject: CreateFile failed, err=5`）。
 *
 * 所以取消之后必须等 IRP 完成；这里用事件对象带超时地等，
 * 兜底避免驱动有 bug 时无限挂住。
 */
static bool _cancelOverlapped(HANDLE deviceHandle, WINUSB_INTERFACE_HANDLE winusb,
                             UCHAR pipe, OVERLAPPED* ov, DWORD waitMs)
{
    if (!ov || !ov->hEvent) {
        return true;
    }

    CancelIoEx(deviceHandle, ov);   // 只取消这一个 IRP，不误伤其它线程

    if (WaitForSingleObject(ov->hEvent, waitMs) == WAIT_OBJECT_0) {
        DWORD dummy = 0;
        GetOverlappedResult(deviceHandle, ov, &dummy, FALSE);
        CloseHandle(ov->hEvent);
        ov->hEvent = NULL;
        return true;
    }

    // 光靠 CancelIoEx 没回来：用 WinUsb_AbortPipe 强制收尾（它是**同步**的，
    // 返回时该管线上的 IRP 一定已经完成）。
    if (winusb && pipe) {
        BulkTrace("  cancel: IRP pending after %lu ms -> WinUsb_AbortPipe(0x%02X)",
                  (unsigned long)waitMs, (unsigned)pipe);
        WinUsb_AbortPipe(winusb, pipe);
        WinUsb_ResetPipe(winusb, pipe);
    }

    if (WaitForSingleObject(ov->hEvent, 3000) == WAIT_OBJECT_0) {
        DWORD dummy = 0;
        GetOverlappedResult(deviceHandle, ov, &dummy, FALSE);
        CloseHandle(ov->hEvent);
        ov->hEvent = NULL;
        return true;
    }

    // 仍然没完成：**绝不能** CloseHandle(事件)、也绝不能释放这段 OVERLAPPED ——
    // 驱动手里还攥着它的指针，释放等于给内核一个悬垂指针（会写坏内存，
    // 且 file object 引用永久不释放 → 接口永久 err=5）。
    // 宁可有意识地泄漏这一次的 OVERLAPPED+事件（几十字节，且此后该通道会重开）。
    BulkTrace("  cancel: IRP STILL pending after abort -> abandon OVERLAPPED on purpose "
              "(never free an IRP the driver still owns)");
    return false;
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

    // 事件对象必须**每次传输独立创建**：_cancelOverlapped 在成功取消时会
    // CloseHandle 它，句柄生命周期与本次 OVERLAPPED 绑定死了 —— 跨传输复用
    // 会在一次超时之后留下悬垂句柄，之后所有传输全部失败（已实测）。
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

    DWORD error = result ? ERROR_SUCCESS : GetLastError();

    if (!result && error == ERROR_IO_PENDING) {
        // 等待操作完成
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_TIMEOUT) {
            // 取消并等 IRP 真正结束；事件对象的收尾（CloseHandle 或有意泄漏）
            // 由 _cancelOverlapped 内部统一处理，这里不能再碰它。
            _cancelOverlapped(g_winusb.deviceHandle, g_winusb.winusbHandle,
                              g_winusb.bulkOutPipe, &overlapped, 3000);

            // ★ 超时后**无条件复位 OUT 端点**。
            //
            // bulk OUT 超时几乎总意味着设备不再读这条管线（一直 NAK，或端点已 halt）。
            // 只做 CancelIoEx 的话端点状态原样保留 → 下一条命令照样超时 → 上层重试
            // 就退化成**死循环**。现场实测（2026-09-30）：一次会话里
            // `bulkWrite OUT ep=0x03 len=64 TIMEOUT after 1000 ms` + `cmd=0x05 write
            // failed (-2)` 连刷 36 次以上、每秒一次，从用户视角就是"程序卡死"。
            //
            // AbortPipe 结束该管线上所有 IRP，ResetPipe 清掉 halt 与 DATA toggle，
            // 让下一条命令有机会真正发出去（而不是必然再次超时）。
            // 注意：复位救不了"设备侧整体卡住"（那需要给调试器重新上电），
            // 但它保证本层**快速如实失败**，而不是无限重试。
            WinUsb_AbortPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);
            WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);

            BulkTrace("  bulkWrite OUT ep=0x%02X len=%u TIMEOUT after %d ms (pipe reset)",
                      (unsigned)g_winusb.bulkOutPipe, (unsigned)len, timeoutMs);
            return -2;  // 超时
        }
        if (!GetOverlappedResult(g_winusb.deviceHandle, &overlapped, &bytesWritten, FALSE)) {
            error = GetLastError();
            CloseHandle(overlapped.hEvent);
            BulkTrace("  bulkWrite OUT ep=0x%02X len=%u FAILED err=%lu",
                      (unsigned)g_winusb.bulkOutPipe, (unsigned)len, (unsigned long)error);
            return -1;
        }
    } else if (!result) {
        CloseHandle(overlapped.hEvent);
        BulkTrace("  bulkWrite OUT ep=0x%02X len=%u FAILED err=%lu",
                  (unsigned)g_winusb.bulkOutPipe, (unsigned)len, (unsigned long)error);
        return -1;
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

    // 事件对象必须**每次传输独立创建**：_cancelOverlapped 在成功取消时会
    // CloseHandle 它，句柄生命周期与本次 OVERLAPPED 绑定死了 —— 跨传输复用
    // 会在一次超时之后留下悬垂句柄，之后所有传输全部失败（已实测）。
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

    DWORD error = result ? ERROR_SUCCESS : GetLastError();

    if (!result && error == ERROR_IO_PENDING) {
        // 等待操作完成
        DWORD waitResult = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (waitResult == WAIT_TIMEOUT) {
            // 必须先等 IRP 真正结束再释放，否则设备接口会被永久占用
            // （详见 _cancelOverlapped 的说明）；事件由它内部收尾。
            _cancelOverlapped(g_winusb.deviceHandle, g_winusb.winusbHandle,
                              g_winusb.bulkInPipe, &overlapped, 3000);
            BulkTrace("  bulkRead IN ep=0x%02X maxLen=%u TIMEOUT after %d ms",
                      (unsigned)g_winusb.bulkInPipe, (unsigned)maxLen, timeoutMs);
            return -2;  // 超时
        }
        if (!GetOverlappedResult(g_winusb.deviceHandle, &overlapped, &bytesRead, FALSE)) {
            error = GetLastError();
            CloseHandle(overlapped.hEvent);
            BulkTrace("  bulkRead IN ep=0x%02X FAILED err=%lu",
                      (unsigned)g_winusb.bulkInPipe, (unsigned long)error);
            return -1;
        }
    } else if (!result) {
        CloseHandle(overlapped.hEvent);
        BulkTrace("  bulkRead IN ep=0x%02X FAILED err=%lu",
                  (unsigned)g_winusb.bulkInPipe, (unsigned long)error);
        return -1;
    }

    CloseHandle(overlapped.hEvent);
    return (int)bytesRead;
}

// 定义在文件后部（用到 _bulkRead）
static void _flushAndDrainPipes(void);
// 定义在文件后部（用到 g_winusb / deviceSpeed）
static size_t _bulkPacketSize(size_t cmdLen);
// 定义在文件后部（用到 _bulkWrite/_bulkRead）
static void _calibrateOutPacket(void);

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

    // 打开后先清端点、排空设备 IN FIFO 里的历史响应，
    // 保证第一条命令读到的就是它自己的响应（见 _flushAndDrainPipes 说明）
    _flushAndDrainPipes();

    // 再用一次只读探测标定"实际能通的出包长度"（见 _calibrateOutPacket）
    _calibrateOutPacket();

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

    // 已经打开时先按"当前偏好"决定复用还是切换模式。
    //
    // AGDI 在一次调试会话里会反复调用 rddi_Open（实测 7 次）。每次
    // "先 Shutdown 再重开"都要重新 CreateFile，而 WinUSB 设备是**独占**的 ——
    // 上一个句柄尚未完全释放时第二次 CreateFile 会得到
    // ERROR_ACCESS_DENIED(5)，随后所有打开都退化成 V1 HID。
    // 实测现象：7 次 RDDI_Open 里只有第 1 次是 V2，其余 6 次全变 V1。
    const int pref = _transportPreference();

    if (g_ctx.initialized) {
        const bool wantBulk = (pref != USB_BULK_TRANSPORT_HID);
        const bool isBulk   = (g_ctx.mode == USB_BULK_BULK_MODE && g_winusb.winusbHandle != nullptr);

        if (wantBulk && isBulk) {
            BulkTrace("Init: reuse existing V2 connection (skip reopen), pref=%s",
                      _transportName(pref));
            return 0;
        }
        if (!wantBulk && !isBulk) {
            BulkTrace("Init: reuse existing V1 HID connection (skip reopen), pref=%s",
                      _transportName(pref));
            return 0;
        }
        if (pref == USB_BULK_TRANSPORT_AUTO && !isBulk) {
            // AUTO 且已经探测过 V2（失败）→ 保持 HID。
            // 否则每次 RDDI_Open 都重试一遍 V2，既慢又会反复触发
            // CreateFile 失败路径（那正是历史上"整场会话退化成 V1"的来源）。
            // 注意：一旦用户在对话框里选定某一条接口，ConfigureInterface 会给出
            // 明确的 pref（BULK/HID），不再走这里；本分支只用于"尚未选定"的窗口期。
            BulkTrace("Init: reuse existing V1 HID connection (V2 already probed)");
            return 0;
        }

        // 模式需要切换（hid 偏好下当前是 V2，或 bulk 偏好下当前是 V1）
        BulkTrace("Init: switching transport (pref=%s, current=%s)",
                  _transportName(pref), isBulk ? "v2" : "v1");
        ORBMDK_USB_Bulk_Shutdown();
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    g_ctx.timeoutMs = 1000;
    g_ctx.timerFrequency = 10000000;  // 10 MHz 默认值

    // 尝试初始化 V2 Bulk 模式（除非显式要求只用 HID）
    if (pref != USB_BULK_TRANSPORT_HID &&
        _initWinUSB(vid ? vid : ORBTRACE_VID, pid ? pid : ORBTRACE_PID, serial)) {
        g_ctx.mode = USB_BULK_BULK_MODE;
        g_ctx.deviceInfo.protocol_version = 2;
        g_ctx.deviceInfo.max_packet_size = 512;  // USB Full Speed Bulk
        g_ctx.deviceInfo.max_packet_count = 1;
        g_ctx.initialized = true;
        BulkTrace("Init: V2 Bulk mode OK (product='%s', serial='%s')",
                  g_bulkProductName, g_bulkSerial);
        return 0;
    }

    // 选了 "CMSIS-DAP v2"（ifNo=0）却打不开：**如实失败，绝不静默降级到 V1**。
    //
    // 静默降级是最坏的处理方式：它把"V2 坏了"伪装成"一切正常"，于是整场会话
    // 都跑在 V1 上、用户看不出任何异常，真正的缺陷（句柄泄漏导致的 err=5）
    // 被永久掩盖 —— 历史上正是这样拖了很久。选谁就必须是谁，打不开就报错。
    if (pref == USB_BULK_TRANSPORT_BULK) {
        BulkTrace("Init: CMSIS-DAP v2 selected but V2 unavailable -> FAIL "
                  "(no silent fallback; see the err=5 notes in _closeWinUSB)");
        return -1;
    }

    // 只有 AUTO（用户尚未选定接口的窗口期）才会走到这里
    BulkTrace("Init: AUTO + V2 Bulk unavailable -> open V1 HID");

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
    BulkTrace("Shutdown: mode=%d, initialized=%d, winusbHandle=%d, deviceHandle=%d",
              (int)g_ctx.mode, (int)g_ctx.initialized,
              g_winusb.winusbHandle ? 1 : 0,
              (g_winusb.deviceHandle && g_winusb.deviceHandle != INVALID_HANDLE_VALUE) ? 1 : 0);
    // ⚠️ 这里**不能**因为 g_ctx.initialized == false 就提前 return。
    //
    // `g_ctx` 与 `g_winusb` 是两份独立状态：句柄完全可能是在 g_ctx 已被清零
    // 之后仍然有效的（Init 中途失败、探测失败回退、外部调用顺序异常等）。
    // 一旦漏关，WinUSB 接口（独占）就**永久**占用，之后所有 CreateFile 都是
    // ERROR_ACCESS_DENIED(5) —— 实测表现为"V2 通道整场会话再也打不开"，
    // 即使重启 Keil 也不行，必须结束进程（甚至设备重新枚举）。
    //
    // 所以只按 g_winusb 的**实际状态**决定要不要关，不看 g_ctx。
    _closeWinUSB();

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

/**
 * @brief V2 出包/入包的字节数
 *
 * CMSIS-DAP v2 规范只要求"一条命令作为一个 USB bulk 传输发出"，**没有**规定
 * 填充长度。固件实现差别很大，所以由开机自标定**实测**决定（见 _calibrateOutPacket）：
 *   1. 标定结果 `g_winusb.alignedOutPkt`（已用真实命令验证过）
 *   2. 未标定时用描述符端点 wMaxPacketSize **减 1**（绝不发整包）
 *
 * 刻意**不留**"手动指定包长"的开关：那个开关正是当初用来绕过包长缺陷的，
 * 留着只会让同类问题再次被掩盖（§17.2 / §17.4）。
 */

static size_t _bulkPacketSize(size_t cmdLen)
{
    // 出包长度 = max(命令长度, 实际包长)。
    //
    // 实际包长优先取**自标定结果**（_calibrateOutPacket，已用真实命令验证过）。
    //
    // 未标定时用**描述符值减 1**：描述符里的 wMaxPacketSize **不能直接用** ——
    // 恰等于它的包在 USB 上不是短包，固件会等下一包，命令永远不被派发
    // （实测 orbtrace 描述符 512：按 512 发时第一条能答、之后永久无响应）。
    // 详见 _calibrateOutPacket 的 ①。
    size_t def;
    if (g_winusb.alignedOutPkt) {
        def = g_winusb.alignedOutPkt;
    } else {
        def = g_winusb.bulkOutMaxPkt;
        if (def > 1) {
            def -= 1;
        }
        if (def == 0) {
            def = 64;                         // 描述符没给出时的保守回退
        }
    }
    if (def < cmdLen) {
        def = cmdLen;
    }
    return def;
}

/**
 * @brief 当前传输模式下"一条命令能占用的最大字节数"（见头文件说明）
 *
 * 用 cmdLen=0 取"有效出包长度"本身：标定值优先，未标定则描述符 wMaxPacketSize-1。
 * 上层只要保证 5 + count*4 <= 该值，_bulkPacketSize 就会把出包长度取成这个
 * **已验证过的值**，不会因为命令变长而变成未验证的长度。
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetMaxCommandBytes(void)
{
    if (!g_ctx.initialized || g_ctx.mode != USB_BULK_BULK_MODE) {
        return 0;                       // V1 HID / 未连接：调用方走 64 字节规则
    }
    return (int)_bulkPacketSize(0);
}

/**
 * @brief 清空端点并排空设备 IN FIFO 里的历史响应
 *
 * 为什么必须做：设备的响应是**流式**的，超时/中断后没被取走的响应会留在
 * 设备 FIFO 里。下一次打开时如果不清，第一条命令就会读到**上一次的响应**，
 * 表现为"响应与命令对不上"（实测：发 DAP_Info 却收到 05 01 77 14）。
 */
static void _flushAndDrainPipes(void)
{
    if (!g_winusb.winusbHandle) {
        return;
    }

    WinUsb_AbortPipe(g_winusb.winusbHandle, g_winusb.bulkInPipe);
    WinUsb_AbortPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);
    WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkInPipe);
    WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkOutPipe);

    uint8_t junk[1024];
    int total = 0;
    int n;
    while ((n = _bulkRead(junk, sizeof(junk), 50)) > 0) {
        total += n;
        if (total > (64 * 1024)) {
            break;                            // 防御：不无限读
        }
    }

    // 排空循环是**以一次超时结束**的，那次读会在 IN 管线上留下一个被取消的
    // IRP；不复位的话，紧随其后的第一条真实命令会白白超时一次（实测）。
    WinUsb_ResetPipe(g_winusb.winusbHandle, g_winusb.bulkInPipe);

    BulkTrace("flush: pipes aborted/reset, drained %d stale byte(s)", total);
}

/**
 * @brief 出包长度标定（每次打开设备做一次）
 *
 * 规则全部来自实测（见 COMPAT_ANALYSIS §16），不再靠猜：
 *
 * ① **绝不发"整包"**：出包长度恰等于端点 wMaxPacketSize 时，在 USB 上它不是
 *    短包，固件（TinyUSB 一类）会认为这次传输还没结束、继续等下一包 ——
 *    命令永远不被派发。实测 orbtrace（描述符 512）：pad=512 时"第一条能答、
 *    之后**永久**无响应"；pad=64 / pad=508 都稳定一问一答。
 *
 * ② **首选设备自报的包长**：`DAP_Info(0xFF)` 返回的就是它愿意接收的最大长度
 *    （orbtrace 报 508 = 512-4，正是为避开 ①）。
 *
 * ③ **自报值也要验证**：发一条可校验的只读命令（`DAP_Info(0xF0)` capabilities，
 *    期望响应 [00, 01, xx]），拿不到合法响应就换下一个候选。
 *
 * ④ **引导查询用 64**（短包，任何 wMaxPacketSize ≥ 64 的设备都收得下），
 *    而且打开后的**第一条命令可能被吞**（实测 pad=64 时如此），所以每个候选
 *    最多重试 3 次。
 *
 * 结果记在 `g_winusb.alignedOutPkt`，后续所有命令统一使用。
 */
static bool _probeRoundTrip(size_t cand, int infoId, uint8_t* rbuf, size_t rcap, int* gotBytes)
{
    uint8_t pkt[1024] = {0};
    pkt[0] = 0x00;                             // ID_DAP_INFO
    pkt[1] = (uint8_t)infoId;
    if (_bulkWrite(pkt, cand, 500) < 0) {
        return false;
    }
    const int n = _bulkRead(rbuf, (ULONG)rcap, 500);
    if (n <= 0) {
        return false;
    }
    *gotBytes = n;
    return true;
}

static void _calibrateOutPacket(void)
{
    if (g_winusb.bulkOutMaxPkt == 0) {
        g_winusb.alignedOutPkt = 64;
        BulkTrace("calibrate: descriptor gave no packet size -> use 64");
        return;
    }

    // ---- ④ 引导查询：用短包问设备"你最大包长是多少"（DAP_Info 0xFF）----
    ULONG devPkt = 0;
    {
        ULONG boot = 64;
        if ((ULONG)g_winusb.bulkOutMaxPkt < boot) {
            boot = (ULONG)g_winusb.bulkOutMaxPkt;   // 描述符比 64 还小就跟着它
        }
        for (int attempt = 0; attempt < 3 && devPkt == 0; ++attempt) {
            uint8_t rbuf[128] = {0};
            int n = 0;
            if (!_probeRoundTrip(boot, 0xFF, rbuf, sizeof(rbuf), &n)) {
                continue;                            // 第一条被吞属于已知现象
            }
            if (n >= 4 && rbuf[0] == 0x00 && rbuf[1] >= 2) {   // [00, 02, lo, hi]
                const ULONG v = (ULONG)rbuf[2] | ((ULONG)rbuf[3] << 8);
                if (v >= 8) {
                    devPkt = v;
                }
            }
        }
        BulkTrace("calibrate: device reports packet size = %u (descriptor=%u, speed=%u)",
                  (unsigned)devPkt, (unsigned)g_winusb.bulkOutMaxPkt,
                  (unsigned)g_winusb.deviceSpeed);
    }

    // ---- ③ 候选验证：谁能稳定答对 DAP_Info(0xF0) 就用谁 ----
    size_t cands[3] = {0, 0, 0};
    int nc = 0;
    if (devPkt >= 8) {
        cands[nc++] = (size_t)devPkt;
    }
    if (nc == 0 || cands[0] != 64) {
        cands[nc++] = 64;                        // 通用安全值（短包）
    }

    for (int i = 0; i < nc; ++i) {
        if (cands[i] == 0) {
            continue;
        }
        if (i > 0) {
            _flushAndDrainPipes();               // 换候选前把通道清干净
        }
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint8_t rbuf[128] = {0};
            int n = 0;
            if (_probeRoundTrip(cands[i], 0xF0, rbuf, sizeof(rbuf), &n) &&
                n >= 3 && rbuf[0] == 0x00 && rbuf[1] == 1) {
                g_winusb.alignedOutPkt = (USHORT)cands[i];
                BulkTrace("calibrate: outPacket=%u verified (DAP_Info 0xF0 -> %02X %02X %02X)",
                          (unsigned)cands[i], rbuf[0], rbuf[1], rbuf[2]);
                _flushAndDrainPipes();
                return;
            }
            BulkTrace("calibrate: packet=%u attempt %d -> no valid response, retry",
                      (unsigned)cands[i], attempt + 1);
        }
    }

    g_winusb.alignedOutPkt = 64;                 // 都不通也要留个可用的值
    _flushAndDrainPipes();
    BulkTrace("calibrate: no packet size verified, falling back to 64");
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetDeviceSpeed(void)
{
    if (!g_ctx.initialized || g_ctx.mode != USB_BULK_BULK_MODE) {
        return 0;                       // 未连接 / V1 HID：速度由 HID 路径自己决定
    }
    return (int)g_winusb.deviceSpeed;   // 1=Low 2=Full 3=High（WinUsb DEVICE_SPEED）
}

/**
 * @brief 一次 V2 命令往返（写一条命令、读一条响应）
 *
 * 返回值与 ORBMDK_HID_DAPCommand 的契约一致：**0 = 成功，<0 = 失败**。
 * 响应的字节数由 *respLen 回传 —— 早期实现把字节数当返回值返回，
 * 于是"成功"被调用方当成"失败"（详见函数末尾说明）。
 */
static int _bulkTransferCommand(const uint8_t* cmd, size_t cmdLen,
                                uint8_t* resp, size_t* respLen, int timeoutMs)
{
    const size_t outLen = _bulkPacketSize(cmdLen);

    uint8_t pkt[1024];
    if (outLen > sizeof(pkt) || cmdLen > sizeof(pkt)) {
        return -1;
    }
    memset(pkt, 0, outLen);
    memcpy(pkt, cmd, cmdLen);

    const int ret = _bulkWrite(pkt, outLen, timeoutMs);
    if (ret < 0) {
        BulkTrace("DAPCommand: cmd=0x%02X write failed (%d)", cmd[0], ret);
        return ret;
    }

    // 响应同样按整包读入，再按调用方缓冲大小截断
    uint8_t rbuf[1024];
    if (outLen > sizeof(rbuf)) {
        return -1;
    }
    const int n = _bulkRead(rbuf, outLen, timeoutMs);
    if (n < 0) {
        BulkTrace("DAPCommand: cmd=0x%02X outLen=%u read failed (%d)",
                  cmd[0], (unsigned)outLen, n);
        return n;
    }

    const size_t copy = ((size_t)n < *respLen) ? (size_t)n : *respLen;
    memcpy(resp, rbuf, copy);
    *respLen = copy;

    return 0;
}

ORBMDK_INTERNAL int ORBMDK_USB_Bulk_DAPCommand(const uint8_t* cmd, size_t cmdLen,
                                          uint8_t* resp, size_t* respLen, int timeoutMs)
{
    if (!g_ctx.initialized) return -1;

    if (g_ctx.mode != USB_BULK_BULK_MODE) {
        // V1 HID 模式
        return ORBMDK_HID_DAPCommand(cmd, cmdLen, resp, respLen, timeoutMs);
    }

    // 最多两次：响应的首字节应当**回显命令号**（CMSIS-DAP 约定）。
    // 对不上说明设备 FIFO 里还有上一次没取走的响应（超时/中断留下的），
    // 排空后重试一次，避免把陈旧数据当成这次的响应交给上层。
    for (int attempt = 0; attempt < 2; ++attempt) {
        const int r = _bulkTransferCommand(cmd, cmdLen, resp, respLen, timeoutMs);
        if (r < 0) {
            return r;
        }
        if (*respLen == 0 || resp[0] == cmd[0]) {
            // ⚠️ 成功必须返回 **0**。
            // 原实现返回 (int)copy（响应的字节数），而调用方
            // ORBMDK_HID_DAPCommand 的契约是"0 = 成功、非 0 = 失败"：
            //     const int r = ORBMDK_USB_Bulk_DAPCommand(...);
            //     if (r != 0) { ...failed...; return r; }
            // 于是任何**成功**的 V2 命令都会被当成失败返回，
            // 上层表现为 DAP_ReadReg 失败 / DAP_ConnectTarget 失败 →
            // AGDI 随即退回 V1，整场会话都用不上 V2。
            return 0;
        }

        if (resp[0] == 0xFF) {
            // 0xFF = 设备明确回答"不支持该命令"（DAPLink 约定；orbtrace 对
            // SWO 类命令即如此）。这不是 FIFO 残留，不必冲刷重试 —— 重试也
            // 只会再拿到一个 0xFF，白白多花一次 flush + 复位的时间。
            BulkTrace("DAPCommand: cmd=0x%02X not supported by device (resp=0xFF)", cmd[0]);
            return -1;
        }

        BulkTrace("DAPCommand: cmd=0x%02X got stale response (resp[0]=0x%02X) -> flush & retry",
                  cmd[0], resp[0]);
        _flushAndDrainPipes();
    }

    BulkTrace("DAPCommand: cmd=0x%02X response mismatch after retry", cmd[0]);
    return -1;
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
