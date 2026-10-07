/**
 * @file ORBMDK_RDDI_FullTest.cpp
 * @brief ORBMDK_RDDI.dll 全面功能测试-AI编写
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

// RDDI 头文件
#include "../include/ORBMDK_RDDI.h"

// ============================================================================
// 函数指针类型定义
// ============================================================================
typedef int(*PFN_RDDI_Open)(RDDIHandle*, const void*);
typedef int(*PFN_RDDI_Close)(RDDIHandle);
typedef int(*PFN_RDDI_GetLastError)(int*, char*, size_t);
typedef int(*PFN_RDDI_SetLogCallback)(RDDIHandle, void(*)(int, const char*));

typedef int(*PFN_CMSIS_DAP_Connect)(RDDIHandle, int*);
typedef int(*PFN_CMSIS_DAP_ResetDAP)(RDDIHandle);
typedef int(*PFN_CMSIS_DAP_DetectNumberOfDevices)(RDDIHandle, int*);
// 注意：ARM/Keil 的实际签名是 3 参 —— (handle, int *idArray, size_t sizeInBytes)，
// 且写入数组的是 **IDCODE**（不是 DAP 索引），sizeInBytes 是**字节数**。
// 旧测试误用 4 参原型 (handle, int *count, char *deviceIDs, int len)，实参整体错位：
// idArray 收到 &count（单个 int 的地址），sizeOfArray 收到 deviceID 指针值（天文数字）
// → 向 &count 后面狂写 IDCODE → 栈被踩坏 → 之后在无关位置崩溃（0xc0000005）。
// 这正是历史上 UV4.exe 崩溃的同一个坑，见 ORBMDK_RDDI.cpp 的说明。
typedef int(*PFN_CMSIS_DAP_GetDeviceIDList)(RDDIHandle, int*, size_t);
typedef int(*PFN_CMSIS_DAP_ConfigureDebugger)(RDDIHandle, const char*);
typedef int(*PFN_CMSIS_DAP_SWJ_Sequence)(RDDIHandle, int, unsigned char*);
typedef int(*PFN_CMSIS_DAP_SWJ_Pins)(RDDIHandle, unsigned char, unsigned char, int*, int);
typedef int(*PFN_CMSIS_DAP_SWJ_Clock)(RDDIHandle, unsigned int);
typedef int(*PFN_CMSIS_DAP_WriteABORT)(RDDIHandle, int, unsigned int);
typedef int(*PFN_CMSIS_DAP_SWD_Configure)(RDDIHandle, uint8_t);
typedef int(*PFN_CMSIS_DAP_SWD_Sequence)(RDDIHandle, int, unsigned char*);
typedef int(*PFN_CMSIS_DAP_JTAG_Configure)(RDDIHandle, int, uint8_t*);
typedef int(*PFN_CMSIS_DAP_JTAG_Sequence)(RDDIHandle, int, uint8_t*, uint8_t*, uint8_t*, uint8_t);
typedef int(*PFN_CMSIS_DAP_JTAG_GetIDCODEs)(RDDIHandle, int*, uint32_t*);
typedef int(*PFN_CMSIS_DAP_Delay)(RDDIHandle, int);

typedef int(*PFN_DAP_ReadReg)(RDDIHandle, int, int, int*);
typedef int(*PFN_DAP_WriteReg)(RDDIHandle, int, int, int);
typedef int(*PFN_DAP_Connect)(RDDIHandle, RDDI_DAP_CONN_DETAILS*);
typedef int(*PFN_DAP_Disconnect)(RDDIHandle);
typedef int(*PFN_DAP_GetInterfaceVersion)(RDDIHandle, int*);
typedef int(*PFN_DAP_Configure)(RDDIHandle, const char*);
// 真实签名：一次传**多个寄存器号**（handle, DAP_ID, numRegs, regIDArray, dataArray）
typedef int(*PFN_DAP_RegReadBlock)(RDDIHandle, int, int, const int*, int*);
typedef int(*PFN_DAP_RegWriteBlock)(RDDIHandle, int, int, const int*, const int*);
typedef int(*PFN_DAP_RegReadRepeat)(RDDIHandle, int, int, int, int*);
typedef int(*PFN_DAP_RegWriteRepeat)(RDDIHandle, int, int, int, const int*);
typedef int(*PFN_DAP_HostStatus)(RDDIHandle, int, int);
// 真实签名 4 参：DAP_Target(handle, const char *request_str, char *resp_str, int resp_len)
typedef int(*PFN_DAP_Target)(RDDIHandle, const char*, char*, int);

typedef int(*PFN_CMSIS_DAP_Capabilities)(RDDIHandle, int, int*);
// 版本号是 int（bits[31:24]=major,[23:16]=minor,[15:0]=build），不是字符串
typedef int(*PFN_CMSIS_DAP_GetInterfaceVersion)(RDDIHandle, int*);
typedef int(*PFN_CMSIS_DAP_GetNumberOfDevices)(RDDIHandle, int*);
typedef int(*PFN_CMSIS_DAP_ResetTarget)(RDDIHandle);
// SWO 签名严格对齐 ARM rddi_dap_swo.h（见 include/ORBMDK_RDDI.h）：
//   旧测试用的是 (handle, uint8_t*)（2 参），而真实是 3 参 —— 实参错位后
//   函数会把寄存器里的垃圾当成第 3 个指针参数并写入 → 0xc0000005。
typedef int(*PFN_CMSIS_DAP_SWO_Control)(RDDIHandle, int);
typedef int(*PFN_CMSIS_DAP_SWO_Status)(RDDIHandle, int*, int*);
// Baudrate 的第二参数同样是**指针**：AGDI 把候选波特率放在栈上、传地址进来
// （µVision 的 SWO 时钟探测循环，见 Todo.md §18.15）。按值声明会让本 DLL
// 解引用一个非法地址 → 0xc0000005。
typedef int(*PFN_CMSIS_DAP_SWO_Baudrate)(RDDIHandle, int*);
typedef int(*PFN_CMSIS_DAP_SWO_Data)(RDDIHandle, int*, void*, int*);

// ============================================================================
// 全局变量
// ============================================================================
static int g_Passed = 0;
static int g_Failed = 0;
static int g_NotImpl = 0;

// ============================================================================
// 日志回调函数
// ============================================================================
static void LogCallback(int level, const char* msg) {
    const char* levelStr = "UNKNOWN";
    switch (level) {
        case 0: levelStr = "DEBUG"; break;
        case 1: levelStr = "INFO";  break;
        case 2: levelStr = "WARN";  break;
        case 3: levelStr = "ERROR"; break;
    }
    // 强制刷新stdout，确保所有日志立即输出
    printf("[%s] %s\n", levelStr, msg);
    fflush(stdout);
}

// ============================================================================
// 测试宏
// ============================================================================
#define TEST(name, expr) do { \
    printf("  %-45s ", name); \
    int result_ = (expr); \
    if (result_ == RDDI_SUCCESS) { \
        printf("PASS (result=%d)\n", result_); \
        g_Passed++; \
    } else { \
        printf("FAIL (result=%d)\n", result_); \
        g_Failed++; \
    } \
} while(0)

#define TEST_NI(name, expr) do { \
    printf("  %-45s ", name); \
    if ((expr) != NULL) { \
        printf("OK\n"); \
    } else { \
        printf("NOT IMPLEMENTED\n"); \
        g_NotImpl++; \
    } \
} while(0)

// ============================================================================
// 主测试程序
// ============================================================================
int main(int argc, char* argv[]) {
    printf("================================================================\n");
    printf("       ORBMDK_RDDI.dll Comprehensive Function Test\n");
    printf("================================================================\n\n");

    // 待测 DLL：默认取"文件名"，LoadLibraryA 会先查 exe 所在目录
    // （build_test.ps1 把 exe 与 dll 都输出到 bin\）；也可用 argv[1] 显式指定。
    const char* dllPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";
    printf("Loading: %s\n\n", dllPath);

    // 加载 DLL
    HMODULE hDLL = LoadLibraryA(dllPath);
    if (!hDLL) {
        printf("FATAL: Cannot load DLL. Error: %d\n", GetLastError());
        return 1;
    }
    printf("OK: DLL loaded\n\n");

    // 获取所有函数指针
    printf("================================================================\n");
    printf("Loading Functions...\n");
    printf("================================================================\n");
    
    PFN_RDDI_Open pfn_RDDI_Open = (PFN_RDDI_Open)GetProcAddress(hDLL, "RDDI_Open");
    PFN_RDDI_Close pfn_RDDI_Close = (PFN_RDDI_Close)GetProcAddress(hDLL, "RDDI_Close");
    PFN_RDDI_GetLastError pfn_RDDI_GetLastError = (PFN_RDDI_GetLastError)GetProcAddress(hDLL, "RDDI_GetLastError");
    PFN_RDDI_SetLogCallback pfn_RDDI_SetLogCallback = (PFN_RDDI_SetLogCallback)GetProcAddress(hDLL, "RDDI_SetLogCallback");
    PFN_CMSIS_DAP_Connect pfn_CMSIS_DAP_Connect = (PFN_CMSIS_DAP_Connect)GetProcAddress(hDLL, "CMSIS_DAP_Connect");
    PFN_CMSIS_DAP_ResetDAP pfn_CMSIS_DAP_ResetDAP = (PFN_CMSIS_DAP_ResetDAP)GetProcAddress(hDLL, "CMSIS_DAP_ResetDAP");
    PFN_CMSIS_DAP_DetectNumberOfDevices pfn_CMSIS_DAP_DetectNumberOfDevices = (PFN_CMSIS_DAP_DetectNumberOfDevices)GetProcAddress(hDLL, "CMSIS_DAP_DetectNumberOfDevices");
    PFN_CMSIS_DAP_GetDeviceIDList pfn_CMSIS_DAP_GetDeviceIDList = (PFN_CMSIS_DAP_GetDeviceIDList)GetProcAddress(hDLL, "CMSIS_DAP_GetDeviceIDList");
    PFN_CMSIS_DAP_ConfigureDebugger pfn_CMSIS_DAP_ConfigureDebugger = (PFN_CMSIS_DAP_ConfigureDebugger)GetProcAddress(hDLL, "CMSIS_DAP_ConfigureDebugger");
    PFN_CMSIS_DAP_SWJ_Sequence pfn_CMSIS_DAP_SWJ_Sequence = (PFN_CMSIS_DAP_SWJ_Sequence)GetProcAddress(hDLL, "CMSIS_DAP_SWJ_Sequence");
    PFN_CMSIS_DAP_SWJ_Pins pfn_CMSIS_DAP_SWJ_Pins = (PFN_CMSIS_DAP_SWJ_Pins)GetProcAddress(hDLL, "CMSIS_DAP_SWJ_Pins");
    PFN_CMSIS_DAP_SWJ_Clock pfn_CMSIS_DAP_SWJ_Clock = (PFN_CMSIS_DAP_SWJ_Clock)GetProcAddress(hDLL, "CMSIS_DAP_SWJ_Clock");
    PFN_CMSIS_DAP_WriteABORT pfn_CMSIS_DAP_WriteABORT = (PFN_CMSIS_DAP_WriteABORT)GetProcAddress(hDLL, "CMSIS_DAP_WriteABORT");
    PFN_CMSIS_DAP_SWD_Configure pfn_CMSIS_DAP_SWD_Configure = (PFN_CMSIS_DAP_SWD_Configure)GetProcAddress(hDLL, "CMSIS_DAP_SWD_Configure");
    PFN_CMSIS_DAP_SWD_Sequence pfn_CMSIS_DAP_SWD_Sequence = (PFN_CMSIS_DAP_SWD_Sequence)GetProcAddress(hDLL, "CMSIS_DAP_SWD_Sequence");
    PFN_CMSIS_DAP_JTAG_Configure pfn_CMSIS_DAP_JTAG_Configure = (PFN_CMSIS_DAP_JTAG_Configure)GetProcAddress(hDLL, "CMSIS_DAP_JTAG_Configure");
    PFN_CMSIS_DAP_JTAG_Sequence pfn_CMSIS_DAP_JTAG_Sequence = (PFN_CMSIS_DAP_JTAG_Sequence)GetProcAddress(hDLL, "CMSIS_DAP_JTAG_Sequence");
    PFN_CMSIS_DAP_JTAG_GetIDCODEs pfn_CMSIS_DAP_JTAG_GetIDCODEs = (PFN_CMSIS_DAP_JTAG_GetIDCODEs)GetProcAddress(hDLL, "CMSIS_DAP_JTAG_GetIDCODEs");
    PFN_CMSIS_DAP_Delay pfn_CMSIS_DAP_Delay = (PFN_CMSIS_DAP_Delay)GetProcAddress(hDLL, "CMSIS_DAP_Delay");
    PFN_DAP_ReadReg pfn_DAP_ReadReg = (PFN_DAP_ReadReg)GetProcAddress(hDLL, "DAP_ReadReg");
    PFN_DAP_WriteReg pfn_DAP_WriteReg = (PFN_DAP_WriteReg)GetProcAddress(hDLL, "DAP_WriteReg");
    PFN_DAP_Connect pfn_DAP_Connect = (PFN_DAP_Connect)GetProcAddress(hDLL, "DAP_Connect");
    PFN_DAP_Disconnect pfn_DAP_Disconnect = (PFN_DAP_Disconnect)GetProcAddress(hDLL, "DAP_Disconnect");
    PFN_DAP_GetInterfaceVersion pfn_DAP_GetInterfaceVersion = (PFN_DAP_GetInterfaceVersion)GetProcAddress(hDLL, "DAP_GetInterfaceVersion");
    PFN_DAP_Configure pfn_DAP_Configure = (PFN_DAP_Configure)GetProcAddress(hDLL, "DAP_Configure");
    PFN_DAP_RegReadBlock pfn_DAP_RegReadBlock = (PFN_DAP_RegReadBlock)GetProcAddress(hDLL, "DAP_RegReadBlock");
    PFN_DAP_RegWriteBlock pfn_DAP_RegWriteBlock = (PFN_DAP_RegWriteBlock)GetProcAddress(hDLL, "DAP_RegWriteBlock");
    PFN_DAP_RegReadRepeat pfn_DAP_RegReadRepeat = (PFN_DAP_RegReadRepeat)GetProcAddress(hDLL, "DAP_RegReadRepeat");
    PFN_DAP_RegWriteRepeat pfn_DAP_RegWriteRepeat = (PFN_DAP_RegWriteRepeat)GetProcAddress(hDLL, "DAP_RegWriteRepeat");
    PFN_DAP_HostStatus pfn_DAP_HostStatus = (PFN_DAP_HostStatus)GetProcAddress(hDLL, "DAP_HostStatus");
    PFN_DAP_Target pfn_DAP_Target = (PFN_DAP_Target)GetProcAddress(hDLL, "DAP_Target");
    PFN_CMSIS_DAP_Capabilities pfn_CMSIS_DAP_Capabilities = (PFN_CMSIS_DAP_Capabilities)GetProcAddress(hDLL, "CMSIS_DAP_Capabilities");
    PFN_CMSIS_DAP_GetInterfaceVersion pfn_CMSIS_DAP_GetInterfaceVersion = (PFN_CMSIS_DAP_GetInterfaceVersion)GetProcAddress(hDLL, "CMSIS_DAP_GetInterfaceVersion");
    PFN_CMSIS_DAP_GetNumberOfDevices pfn_CMSIS_DAP_GetNumberOfDevices = (PFN_CMSIS_DAP_GetNumberOfDevices)GetProcAddress(hDLL, "CMSIS_DAP_GetNumberOfDevices");
    PFN_CMSIS_DAP_ResetTarget pfn_CMSIS_DAP_ResetTarget = (PFN_CMSIS_DAP_ResetTarget)GetProcAddress(hDLL, "CMSIS_DAP_ResetTarget");
    PFN_CMSIS_DAP_SWO_Control pfn_CMSIS_DAP_SWO_Control = (PFN_CMSIS_DAP_SWO_Control)GetProcAddress(hDLL, "CMSIS_DAP_SWO_Control");
    PFN_CMSIS_DAP_SWO_Status pfn_CMSIS_DAP_SWO_Status = (PFN_CMSIS_DAP_SWO_Status)GetProcAddress(hDLL, "CMSIS_DAP_SWO_Status");
    PFN_CMSIS_DAP_SWO_Baudrate pfn_CMSIS_DAP_SWO_Baudrate = (PFN_CMSIS_DAP_SWO_Baudrate)GetProcAddress(hDLL, "CMSIS_DAP_SWO_Baudrate");
    PFN_CMSIS_DAP_SWO_Data pfn_CMSIS_DAP_SWO_Data = (PFN_CMSIS_DAP_SWO_Data)GetProcAddress(hDLL, "CMSIS_DAP_SWO_Data");

    printf("  %-45s %s\n", "RDDI_Open", pfn_RDDI_Open ? "OK" : "MISSING");
    printf("  %-45s %s\n", "RDDI_Close", pfn_RDDI_Close ? "OK" : "MISSING");
    printf("  %-45s %s\n", "RDDI_GetLastError", pfn_RDDI_GetLastError ? "OK" : "MISSING");
    printf("  %-45s %s\n", "RDDI_SetLogCallback", pfn_RDDI_SetLogCallback ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_Connect", pfn_CMSIS_DAP_Connect ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_ResetDAP", pfn_CMSIS_DAP_ResetDAP ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_DetectNumberOfDevices", pfn_CMSIS_DAP_DetectNumberOfDevices ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_GetDeviceIDList", pfn_CMSIS_DAP_GetDeviceIDList ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_ConfigureDebugger", pfn_CMSIS_DAP_ConfigureDebugger ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWJ_Sequence", pfn_CMSIS_DAP_SWJ_Sequence ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWJ_Pins", pfn_CMSIS_DAP_SWJ_Pins ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWJ_Clock", pfn_CMSIS_DAP_SWJ_Clock ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_WriteABORT", pfn_CMSIS_DAP_WriteABORT ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWD_Configure", pfn_CMSIS_DAP_SWD_Configure ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWD_Sequence", pfn_CMSIS_DAP_SWD_Sequence ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_JTAG_Configure", pfn_CMSIS_DAP_JTAG_Configure ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_JTAG_Sequence", pfn_CMSIS_DAP_JTAG_Sequence ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_JTAG_GetIDCODEs", pfn_CMSIS_DAP_JTAG_GetIDCODEs ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_Delay", pfn_CMSIS_DAP_Delay ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_ReadReg", pfn_DAP_ReadReg ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_WriteReg", pfn_DAP_WriteReg ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_Connect", pfn_DAP_Connect ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_Disconnect", pfn_DAP_Disconnect ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_GetInterfaceVersion", pfn_DAP_GetInterfaceVersion ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_Configure", pfn_DAP_Configure ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_RegReadBlock", pfn_DAP_RegReadBlock ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_RegWriteBlock", pfn_DAP_RegWriteBlock ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_RegReadRepeat", pfn_DAP_RegReadRepeat ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_RegWriteRepeat", pfn_DAP_RegWriteRepeat ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_HostStatus", pfn_DAP_HostStatus ? "OK" : "MISSING");
    printf("  %-45s %s\n", "DAP_Target", pfn_DAP_Target ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_Capabilities", pfn_CMSIS_DAP_Capabilities ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_GetInterfaceVersion", pfn_CMSIS_DAP_GetInterfaceVersion ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_GetNumberOfDevices", pfn_CMSIS_DAP_GetNumberOfDevices ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_ResetTarget", pfn_CMSIS_DAP_ResetTarget ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWO_Control", pfn_CMSIS_DAP_SWO_Control ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWO_Status", pfn_CMSIS_DAP_SWO_Status ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWO_Baudrate", pfn_CMSIS_DAP_SWO_Baudrate ? "OK" : "MISSING");
    printf("  %-45s %s\n", "CMSIS_DAP_SWO_Data", pfn_CMSIS_DAP_SWO_Data ? "OK" : "MISSING");
    
    printf("\n");

    // =========================================================================
    // Test 1: RDDI Open/Close
    // =========================================================================
    printf("================================================================\n");
    printf("Test 1: RDDI Core Functions\n");
    printf("================================================================\n");
    
    RDDIHandle handle = 0;
    TEST("RDDI_Open", pfn_RDDI_Open(&handle, NULL));
    
    if (handle == 0 && pfn_RDDI_Open != NULL) {
        // Try with handle 1
        handle = 1;
    }
    
    if (pfn_RDDI_SetLogCallback)
        pfn_RDDI_SetLogCallback(handle, LogCallback);
    
    // Re-open with correct handle
    TEST("RDDI_Open", pfn_RDDI_Open(&handle, NULL));
    if (handle == 0) handle = 1;
    
    TEST("RDDI_Close(handle)", pfn_RDDI_Close(handle));

    // =========================================================================
    // Test 2: CMSIS-DAP Connection
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 2: CMSIS-DAP Connection\n");
    printf("================================================================\n");
    
    pfn_RDDI_Open(&handle, NULL);
    if (handle == 0) handle = 1;
    
    // Detect devices
    int deviceCount = 0;
    TEST("CMSIS_DAP_DetectNumberOfDevices", pfn_CMSIS_DAP_DetectNumberOfDevices(handle, &deviceCount));
    printf("    Device count: %d\n", deviceCount);
    
    // Get device ID list（3 参：数组 + 字节数；元素是 IDCODE）
    int deviceIDs[64] = {0};
    TEST("CMSIS_DAP_GetDeviceIDList", pfn_CMSIS_DAP_GetDeviceIDList(handle, deviceIDs, sizeof(deviceIDs)));
    printf("    Device ID[0]: 0x%08X\n", deviceIDs[0]);
    
    // Get capabilities
    int caps = 0;
    if (pfn_CMSIS_DAP_Capabilities)
        pfn_CMSIS_DAP_Capabilities(handle, 0, &caps);
    printf("    Capabilities: 0x%X\n", caps);
    printf("    - SWD: %s\n", (caps & 0x01) ? "YES" : "NO");
    printf("    - JTAG: %s\n", (caps & 0x02) ? "YES" : "NO");
    printf("    - SWO UART: %s\n", (caps & 0x04) ? "YES" : "NO");
    printf("    - SWO Manchester: %s\n", (caps & 0x08) ? "YES" : "NO");
    printf("    - Atomic: %s\n", (caps & 0x10) ? "YES" : "NO");
    // 0x40 = SWO_STREAMING_TRACE：本层按探针自报能力收口（Todo.md 续 9），
    // 探针不报 SWO 时这两位应为 NO —— 若为 YES 说明中间层又在替固件宣称能力。
    printf("    - SWO Streaming: %s\n", (caps & 0x40) ? "YES" : "NO");
    
    // Get interface version（2 参，版本号是 int 而不是字符串）
    int ifVersion = 0;
    if (pfn_CMSIS_DAP_GetInterfaceVersion)
        pfn_CMSIS_DAP_GetInterfaceVersion(handle, &ifVersion);
    printf("    Interface Version: %d.%d.%d (0x%08X)\n",
           (ifVersion >> 24) & 0xFF, (ifVersion >> 16) & 0xFF, ifVersion & 0xFFFF, ifVersion);

    // =========================================================================
    // Test 3: Host Status
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 3: Host Status\n");
    printf("================================================================\n");
    
    // NULL 数组应被拒绝（RDDI_BADARG = 0x0D），不能返回 0
    TEST("CMSIS_DAP_GetDeviceIDList(NULL buffer) rejected",
         pfn_CMSIS_DAP_GetDeviceIDList(handle, NULL, 0) != 0);
    
    // =========================================================================
    // Test 4: SWD/SWJ Initialization (MUST be before register access!)
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 4: SWD/SWJ Initialization\n");
    printf("================================================================\n");
    
    // 4.1 Connect to DAP
    int connectedInterface = 0;
    if (pfn_DAP_Configure) {
        TEST("DAP_Configure(SWD)", pfn_DAP_Configure(handle, "SWD"));
    }
    TEST("CMSIS_DAP_Connect", pfn_CMSIS_DAP_Connect(handle, &connectedInterface));
    printf("    Connected Interface: %d (%s)\n", connectedInterface, 
           connectedInterface == 1 ? "SWD" : connectedInterface == 2 ? "JTAG" : "Unknown");
    
    // 4.2 Set SWD Clock BEFORE any SWJ sequences (use lower frequency for stability)
    if (pfn_CMSIS_DAP_SWJ_Clock) {
        TEST("CMSIS_DAP_SWJ_Clock(1MHz)", pfn_CMSIS_DAP_SWJ_Clock(handle, 1000000));
    }
    
    // 4.3 Configure SWD protocol
    if (pfn_CMSIS_DAP_SWD_Configure) {
        TEST("CMSIS_DAP_SWD_Configure(0)", pfn_CMSIS_DAP_SWD_Configure(handle, 0));
    }
    
    // 注意：orbtrace 固件的 DAP_Connect(port=SWD) 内部已自动执行 JTAG-to-SWD
    // 切换序列（dbgIF.v 中 CMD_SET_SWD 的 modeshift=0xE79E），因此这里不需要
    // 再手动发送 SWJ_Sequence，否则会破坏已建立的 SWD 连接。
    // 且 CMSIS_DAP_SWJ_Sequence 的 num 参数是"位数"，不能用 sizeof() 字节数。

    // 4.4 Delay for target to settle
    if (pfn_CMSIS_DAP_Delay) {
        TEST("CMSIS_DAP_Delay(100us)", pfn_CMSIS_DAP_Delay(handle, 100));
    }
    
    // =========================================================================
    // Test 5: DAP Register Access (AFTER SWD init!)
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 5: DAP Register Access\n");
    printf("================================================================\n");
    
    if (pfn_DAP_ReadReg) {
        int value = 0;
        
        // 5.1 Read DP IDCODE (should return valid ARM JTAG ID)
        TEST("DAP_ReadReg(DP_IDCODE)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_IDCODE, &value));
        printf("    IDCODE: 0x%08X\n", value);
        if (value == 0 || value == 0xFFFFFFFF) {
            printf("    [WARNING] IDCODE invalid - check SWD connection!\n");
        }
        
        // 5.2 Read DP CTRL_STAT
        TEST("DAP_ReadReg(DP_CTRL_STAT)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_CTRL_STAT, &value));
        printf("    CTRL_STAT: 0x%08X\n", value);
        printf("    - CDBGPWRUPREQ: %s\n", (value & 0x40000000) ? "YES" : "NO");
        printf("    - CDBGPWRUPACK: %s\n", (value & 0x80000000) ? "YES" : "NO");
        
        // 5.3 Enable debug power
        if (pfn_DAP_WriteReg) {
            TEST("DAP_WriteReg(DP_CTRL_STAT=0x50000000)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_DP_CTRL_STAT, 0x50000000));
            // Read back to clear sticky flags
            TEST("DAP_ReadReg(DP_RDBUFF)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_RDBUFF, &value));
            printf("    After power-up CTRL_STAT: 0x%08X\n", value);
        }
        
        // 5.4 Configure AP SELECT (bank 0)
        if (pfn_DAP_WriteReg) {
            TEST("DAP_WriteReg(DP_SELECT=0x00000000)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_DP_SELECT, 0x00000000));
            // Read back
            TEST("DAP_ReadReg(DP_RDBUFF)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_RDBUFF, &value));
        }
        
        // 5.5 Read AP IDR (Access Port ID Register)
        TEST("DAP_ReadReg(AP_IDR)", pfn_DAP_ReadReg(handle, 0, DAP_REG_AP_IDR, &value));
        printf("    AP IDR: 0x%08X\n", value);
        if (value == 0 || value == 0xFFFFFFFF) {
            printf("    [WARNING] AP IDR invalid - check AP access!\n");
        }
    }
    
    // =========================================================================
    // Test 6: Target Reset
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 6: Target Reset\n");
    printf("================================================================\n");
    
    if (pfn_CMSIS_DAP_ResetDAP) {
        TEST("CMSIS_DAP_ResetDAP", pfn_CMSIS_DAP_ResetDAP(handle));
    }
    
    if (pfn_CMSIS_DAP_ResetTarget) {
        TEST("CMSIS_DAP_ResetTarget", pfn_CMSIS_DAP_ResetTarget(handle));
    }
    
    // Reset 之后目标复位、SWD 连接断开，需先等待目标复位稳定再重新连接
    if (pfn_CMSIS_DAP_Delay) {
        pfn_CMSIS_DAP_Delay(handle, 10000);  // 10ms 等待目标复位稳定
    }
    int reconnectedInterface = 0;
    TEST("CMSIS_DAP_Connect (after reset)", pfn_CMSIS_DAP_Connect(handle, &reconnectedInterface));
    printf("    Reconnected Interface: %d (%s)\n", reconnectedInterface,
           reconnectedInterface == 1 ? "SWD" : reconnectedInterface == 2 ? "JTAG" : "Unknown");
    
    // =========================================================================
    // Test 7: Timing Functions
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 7: Timing Functions\n");
    printf("================================================================\n");
    
    if (pfn_CMSIS_DAP_Delay) {
        TEST("CMSIS_DAP_Delay(100us)", pfn_CMSIS_DAP_Delay(handle, 100));
        TEST("CMSIS_DAP_Delay(1000us)", pfn_CMSIS_DAP_Delay(handle, 1000));
        TEST("CMSIS_DAP_Delay(10000us)", pfn_CMSIS_DAP_Delay(handle, 10000));
    } else {
        printf("  CMSIS_DAP_Delay: NOT IMPLEMENTED\n");
        g_NotImpl++;
    }

    // =========================================================================
    // Test 8: SWO Functions
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 8: SWO Functions\n");
    printf("================================================================\n");
    
    if (pfn_CMSIS_DAP_SWO_Control) {
        TEST("CMSIS_DAP_SWO_Control(0)", pfn_CMSIS_DAP_SWO_Control(handle, 0));
    }
    
    if (pfn_CMSIS_DAP_SWO_Baudrate) {
        int swo_baud = 115200;   // 传地址，不是值
        TEST("CMSIS_DAP_SWO_Baudrate(115200)", pfn_CMSIS_DAP_SWO_Baudrate(handle, &swo_baud));
    }
    
    // 3 参：(handle, int *count, int *status)
    int swo_count = 0;
    int swo_status = 0;
    if (pfn_CMSIS_DAP_SWO_Status) {
        TEST("CMSIS_DAP_SWO_Status", pfn_CMSIS_DAP_SWO_Status(handle, &swo_count, &swo_status));
        printf("    SWO Status: count=%d status=0x%02X\n", swo_count, swo_status);
    }

    // =========================================================================
    // Test 9: Block Register Access
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 9: Block Register Access\n");
    printf("================================================================\n");
    
    if (pfn_DAP_RegReadBlock && pfn_DAP_RegWriteBlock) {
        int write_data[4] = {0x12345678, 0x9ABCDEF0, 0x11111111, 0x22222222};
        int read_data[4] = {0};
        
        // Reset 后目标复位、DP 掉电，先确认 SWD 连接并重新上电（对齐 Test 5 流程）
        if (pfn_DAP_ReadReg) {
            int idcode = 0;
            TEST("DAP_ReadReg(DP_IDCODE)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_IDCODE, &idcode));
            printf("    IDCODE after reset: 0x%08X\n", idcode);
        }
        if (pfn_DAP_WriteReg) {
            TEST("DAP_WriteReg(DP_CTRL_STAT=0x50000000)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_DP_CTRL_STAT, 0x50000000));
            int dummy = 0;
            TEST("DAP_ReadReg(DP_RDBUFF)", pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_RDBUFF, &dummy));
        }
        
        // 配置 AP SELECT 选择 AP 0, Bank 0
        if (pfn_DAP_WriteReg) {
            TEST("DAP_WriteReg(DP_SELECT=0x00000000)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_DP_SELECT, 0x00000000));
            // 读取 RDBUFF 清除之前的挂起状态
            int dummy = 0;
            TEST("DAP_ReadReg(DP_RDBUFF)", 
                 pfn_DAP_ReadReg(handle, 0, DAP_REG_DP_RDBUFF, &dummy));
        }
        
        // 配置 AP CSW（32-bit word + auto-increment）与 TAR（STM32F1 SRAM 起始地址）
        if (pfn_DAP_WriteReg) {
            TEST("DAP_WriteReg(AP_CSW=0x23000052)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_AP_CSW, 0x23000052));
            TEST("DAP_WriteReg(AP_TAR=0x20000000)", 
                 pfn_DAP_WriteReg(handle, 0, DAP_REG_AP_TAR, 0x20000000));
        }
        
        printf("    Write data: 0x%08X 0x%08X 0x%08X 0x%08X\n", 
               write_data[0], write_data[1], write_data[2], write_data[3]);
        printf("    >>> RegWriteBlock: DAP_ID=0, reg=0x%08X (AP_DRW), count=4\n",
               DAP_REG_AP_DRW);

        // DAP_RegWriteBlock/ReadBlock 的真实签名是：
        //     (handle, DAP_ID, numRegs, const int *regIDArray, int *dataArray)
        // 即一次传**多个寄存器号**；"同一个寄存器重复 N 次"是 Reg*Repeat。
        // 旧测试按 (handle, DAP_ID, regId, dataArray, count) 调用 —— 参数整体错位：
        // dataArray 收到的是字面量 4 → 被当成指针解引用 → 0xc0000005。
        int ap_reg_ids[4] = { DAP_REG_AP_DRW, DAP_REG_AP_DRW,
                              DAP_REG_AP_DRW, DAP_REG_AP_DRW };

        // 写入 AP_DRW - AP Bank 0
        int writeResult9 = pfn_DAP_RegWriteBlock(handle, 0, 4, ap_reg_ids, write_data);
        printf("    <<< RegWriteBlock result: %d\n", writeResult9);
        if (writeResult9 == RDDI_SUCCESS) {
            printf("    PASS\n");
            g_Passed++;
        } else {
            printf("    FAIL\n");
            g_Failed++;
        }
        
        printf("    >>> RegReadBlock: DAP_ID=0, reg=0x%08X (AP_DRW), count=4\n",
               DAP_REG_AP_DRW);
        
        // 读之前重置 TAR（写入后 TAR 已自增，需回到起始地址）
        if (pfn_DAP_WriteReg) {
            pfn_DAP_WriteReg(handle, 0, DAP_REG_AP_TAR, 0x20000000);
        }
        
        // 读取 AP_DRW - 读回 TAR 指向地址的数据
        int readResult9 = pfn_DAP_RegReadBlock(handle, 0, 4, ap_reg_ids, read_data);
        printf("    <<< RegReadBlock result: %d\n", readResult9);
        if (readResult9 == RDDI_SUCCESS) {
            printf("    PASS\n");
            g_Passed++;
        } else {
            printf("    FAIL\n");
            g_Failed++;
        }
        
        printf("    Read data:  0x%08X 0x%08X 0x%08X 0x%08X\n", 
               read_data[0], read_data[1], read_data[2], read_data[3]);
        
        // 验证数据一致性
        int match = 0;
        for (int i = 0; i < 4; i++) {
            if (read_data[i] == write_data[i]) match++;
        }
        printf("    Match: %d/4 words\n", match);
        if (match != 4) {
            printf("    [WARNING] Data mismatch detected!\n");
            g_Failed++;
        }
    } else {
        printf("  DAP_RegReadBlock/DAP_RegWriteBlock: NOT IMPLEMENTED\n");
        g_NotImpl++;
    }
    
    // =========================================================================
    // Test 10: Repeat Register Access
    // =========================================================================
    printf("\n================================================================\n");
    printf("Test 10: Repeat Register Access\n");
    printf("================================================================\n");
    
    if (pfn_DAP_RegReadRepeat && pfn_DAP_RegWriteRepeat) {
        int write_data[4] = {0xAAAAAAAA, 0x55555555, 0xAAAAAAAA, 0x55555555};
        int read_data[4] = {0};
        
        printf("    Write data: 0x%08X 0x%08X 0x%08X 0x%08X\n",
               write_data[0], write_data[1], write_data[2], write_data[3]);
        printf("    >>> RegWriteRepeat: DAP_ID=0, reg=0x%08X (AP_DRW), count=4\n",
               DAP_REG_AP_DRW);

        // 重新写 TAR（Test 9 块读写后 TAR 已自增，需复位到 SRAM 起始地址）
        if (pfn_DAP_WriteReg) {
            pfn_DAP_WriteReg(handle, 0, DAP_REG_AP_TAR, 0x20000000);
        }

        // 使用 Transfer 命令批量读写同一寄存器 (AP_DRW)
        int writeResult10 = pfn_DAP_RegWriteRepeat(handle, 0, 4, DAP_REG_AP_DRW, write_data);
        printf("    <<< RegWriteRepeat result: %d\n", writeResult10);
        if (writeResult10 == RDDI_SUCCESS) {
            printf("    PASS\n");
            g_Passed++;
        } else {
            printf("    FAIL\n");
            g_Failed++;
        }

        printf("    >>> RegReadRepeat: DAP_ID=0, reg=0x%08X (AP_DRW), count=4\n",
               DAP_REG_AP_DRW);
        // 读之前重置 TAR
        if (pfn_DAP_WriteReg) {
            pfn_DAP_WriteReg(handle, 0, DAP_REG_AP_TAR, 0x20000000);
        }
        int readResult10 = pfn_DAP_RegReadRepeat(handle, 0, 4, DAP_REG_AP_DRW, read_data);
        printf("    <<< RegReadRepeat result: %d\n", readResult10);
        if (readResult10 == RDDI_SUCCESS) {
            printf("    PASS\n");
            g_Passed++;
        } else {
            printf("    FAIL\n");
            g_Failed++;
        }

        printf("    Read data:  0x%08X 0x%08X 0x%08X 0x%08X\n",
               read_data[0], read_data[1], read_data[2], read_data[3]);
        
        // 验证数据一致性
        int match = 0;
        for (int i = 0; i < 4; i++) {
            if (read_data[i] == write_data[i]) match++;
        }
        printf("    Match: %d/4 words\n", match);
        if (match != 4) {
            printf("    [WARNING] Data mismatch detected!\n");
            g_Failed++;
        }
    } else {
        printf("  DAP_RegReadRepeat/DAP_RegWriteRepeat: NOT IMPLEMENTED\n");
        g_NotImpl++;
    }

    // =========================================================================
    // Cleanup
    // =========================================================================
    printf("\n================================================================\n");
    printf("Cleanup\n");
    printf("================================================================\n");
    
    TEST("DAP_Disconnect", pfn_DAP_Disconnect ? pfn_DAP_Disconnect(handle) : RDDI_SUCCESS);
    TEST("RDDI_Close", pfn_RDDI_Close(handle));
    
    FreeLibrary(hDLL);
    printf("\nDLL unloaded.\n");

    // =========================================================================
    // Summary
    // =========================================================================
    printf("\n================================================================\n");
    printf("                        TEST SUMMARY\n");
    printf("================================================================\n");
    printf("  PASSED:         %d\n", g_Passed);
    printf("  FAILED:         %d\n", g_Failed);
    printf("  NOT IMPLEMENTED: %d\n", g_NotImpl);
    printf("  TOTAL:          %d\n", g_Passed + g_Failed + g_NotImpl);
    printf("================================================================\n");
    
    if (g_Failed == 0) {
        printf("  STATUS: ALL TESTS PASSED!\n");
    } else {
        printf("  STATUS: SOME TESTS FAILED!\n");
    }
    printf("================================================================\n");

    return g_Failed > 0 ? 1 : 0;
}
