/**
 * @file ORBMDK_HID.cpp
 * @brief USB HID 通信层实现
 */

#include "pch.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_HID.h"

#include <hidsdi.h>
#include <setupapi.h>
#include <vector>
#include <string>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// ============================================================================
// Local Logging
// ============================================================================
// 日志级别: 0=DEBUG 1=INFO 2=WARN 3=ERROR
//
// 默认阈值 ERROR：作为被 Keil/AGDI 加载的 DLL，逐条寄存器/传输日志会造成
// 巨量噪声（每次 DAP_Transfer 一条十六进制转储）。仅在排障时调低阈值。
//
// 排障时可用环境变量临时恢复（0=DEBUG 1=INFO 2=WARN 3=ERROR），无需重新编译：
//     set ORBMDK_LOG_LEVEL=0
static int HID_LogLevel(void)
{
    static const int level = []() -> int {
        const char* env = getenv("ORBMDK_LOG_LEVEL");
        if (env && *env) {
            int lvl = atoi(env);
            if (lvl >= 0 && lvl <= 3) {
                return lvl;
            }
        }
        return 3;  // 默认: 仅输出 ERROR
    }();
    return level;
}

static void HID_Log(int level, const char* module, const char* fmt, ...) {
    if (level < HID_LogLevel()) {
        return;  // 被过滤时不构造字符串，避免无谓开销
    }

    char buffer[512] = {};
    int offset = snprintf(buffer, sizeof(buffer), "[ORBMDK][%s] ", module);
    if (offset < 0 || offset >= (int)sizeof(buffer)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer + offset, sizeof(buffer) - offset, fmt, args);
    va_end(args);

    printf("%s\n", buffer);
    OutputDebugStringA(buffer);
}

#define LOG_HID_DEBUG(fmt, ...) HID_Log(0, "HID", fmt, ##__VA_ARGS__)
#define LOG_HID_INFO(fmt, ...)  HID_Log(1, "HID", fmt, ##__VA_ARGS__)
#define LOG_HID_WARN(fmt, ...)  HID_Log(2, "HID", fmt, ##__VA_ARGS__)
#define LOG_HID_ERROR(fmt, ...) HID_Log(3, "HID", fmt, ##__VA_ARGS__)

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

    strcpy_s(g_firmwareVersion, "1.0.0");
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
    std::lock_guard<std::mutex> lock(g_hidMutex);
    if (!g_isConnected) return -1;

    if (product) strncpy_s(product, productLen, g_productName, productLen - 1);
    if (serial) strncpy_s(serial, serialLen, g_serialNumber, serialLen - 1);
    if (version) strncpy_s(version, versionLen, g_firmwareVersion, versionLen - 1);
    return 0;
}

#include "ORBMDK_USB_Bulk.h"   // V2 Bulk 传输（本函数是两层共用的分发点）

int ORBMDK_HID_DAPCommand(const uint8_t* cmd, size_t cmdLen,
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

    if (!g_isConnected || g_hDevice == INVALID_HANDLE_VALUE) return -1;

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

    uint8_t resp[64 * 4 + 4] = {};
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
    uint8_t cmd[1] = {ID_DAP_RESET_TARGET};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    return ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 5000);
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

int DAP_JTAG_Configure(uint8_t irLength, uint8_t devCount)
{
    uint8_t cmd[3] = {ID_DAP_JTAG_CONFIGURE, irLength, devCount};
    uint8_t resp[4] = {};
    size_t respLen = sizeof(resp);
    int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 1000);
    if (result != 0 || respLen < 3) return -1;
    // 响应格式：[报告ID][命令ID][状态]
    return resp[2] == 0 ? 0 : -1;
}

int DAP_JTAG_Sequence(uint8_t sequenceInfo, uint8_t count, const uint8_t* tdiData, uint8_t* tdoData)
{
    if (count == 0) return -1;

    std::vector<uint8_t> cmd;
    cmd.push_back(ID_DAP_JTAG_SEQUENCE);
    cmd.push_back(sequenceInfo);
    cmd.push_back(count);

    // Calculate bytes needed for TDI data
    size_t tdiBytes = (count + 7) / 8;
    if (tdiData) {
        cmd.insert(cmd.end(), tdiData, tdiData + tdiBytes);
    } else {
        cmd.insert(cmd.end(), tdiBytes, 0);
    }

    uint8_t resp[64] = {};
    size_t respLen = sizeof(resp);

    int result = ORBMDK_HID_DAPCommand(cmd.data(), cmd.size(), resp, &respLen, 1000);
    if (result != 0 || respLen < 2) return -1;

    // Extract TDO data if provided
    // 响应（含报告ID）：[报告ID][命令ID][TDO 数据...]
    if (tdoData && (sequenceInfo & 0x01) && respLen > 2) {
        size_t tdoBytes = respLen - 2;
        memcpy(tdoData, &resp[2], std::min(tdoBytes, tdiBytes));
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
            ret = DAP_ResetTarget();
            *result = (ret == 0) ? 0 : -1;
            break;
        default:
            return -1;
    }
    return 0;
}

// ============================================================================
// Streaming Trace Operations
// ============================================================================

static std::mutex g_traceMutex;
static bool g_traceInitialized = false;
static bool g_traceRunning = false;
static std::vector<uint8_t> g_traceBuffer;
static const size_t kTraceBufferSize = 64 * 1024;  // 64KB trace buffer

int StreamingTrace_Init(void)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (g_traceInitialized) return 0;

    g_traceBuffer.reserve(kTraceBufferSize);
    g_traceInitialized = true;
    g_traceRunning = false;
    return 0;
}

void StreamingTrace_Shutdown(void)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    g_traceRunning = false;
    g_traceBuffer.clear();
    g_traceInitialized = false;
}

int StreamingTrace_Start(uint8_t mode)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (!g_traceInitialized) {
        StreamingTrace_Init();
    }

    // 配置 SWO 传输
    // mode: 0 = none, 1 = SWO, 2 = ETM
    if (mode == 1) {
        // 启用 SWO 传输
        DAP_SWO_Transport(1);  // 0=none, 1=SWO
        DAP_SWO_Control(1, NULL);  // Start
        g_traceRunning = true;
        return 0;
    }

    return -1;
}

int StreamingTrace_Stop(void)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (!g_traceRunning) return 0;

    // 停止 SWO
    DAP_SWO_Control(0, NULL);  // Stop
    g_traceRunning = false;
    return 0;
}

int StreamingTrace_GetData(uint8_t* buffer, size_t* size, uint32_t timeoutMs)
{
    std::lock_guard<std::mutex> lock(g_traceMutex);
    if (!g_traceRunning || !buffer || !size || *size == 0) {
        return -1;
    }

    // 读取 SWO 数据
    size_t requestedSize = *size;
    size_t totalRead = 0;

    while (totalRead < requestedSize && g_traceRunning) {
        size_t readSize = requestedSize - totalRead;
        if (readSize > 512) readSize = 512;

        uint8_t resp[512 + 5] = {};
        size_t respLen = sizeof(resp);

        uint8_t cmd[3] = {
            ID_DAP_SWO_DATA,
            static_cast<uint8_t>(readSize & 0xFF),
            static_cast<uint8_t>((readSize >> 8) & 0xFF),
        };

        int result = ORBMDK_HID_DAPCommand(cmd, sizeof(cmd), resp, &respLen, 100);
        if (result != 0 || respLen < 5) break;

        size_t dataLen = respLen - 5;
        memcpy(buffer + totalRead, &resp[5], dataLen);
        totalRead += dataLen;

        if (dataLen == 0) {
            // 没有数据，稍微等待
            Sleep(1);
        }
    }

    *size = totalRead;
    return 0;
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
    if (index < 0 || index >= 2) return -1;

    if (name && nameLen > 0) {
        if (index == 0) {
            strncpy_s(name, nameLen, "SWO", nameLen - 1);
        } else {
            strncpy_s(name, nameLen, "ETM", nameLen - 1);
        }
    }

    if (type && typeLen > 0) {
        if (index == 0) {
            strncpy_s(type, typeLen, "UART", typeLen - 1);
        } else {
            strncpy_s(type, typeLen, "ARM_ETM", typeLen - 1);
        }
    }

    return 0;
}

} // namespace ORBMDK
