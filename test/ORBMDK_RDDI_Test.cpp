/**
 * @file ORBMDK_RDDI_Test.cpp
 * @brief ORBMDK_RDDI.dll 虚拟调用测试程序
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>

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
typedef int(*PFN_CMSIS_DAP_GetDeviceIDList)(RDDIHandle, int*, char*, int);
typedef int(*PFN_DAP_GetSupportedHostStatusIDs)(RDDIHandle, int*, int*);
typedef int(*PFN_CMSIS_DAP_ConfigureDebugger)(RDDIHandle, const char*);
typedef int(*PFN_DAP_ReadReg)(RDDIHandle, int, int, int*);

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
    printf("[%s] %s\n", levelStr, msg);
}

// ============================================================================
// 主测试程序
// ============================================================================
int main(int argc, char* argv[]) {
    printf("========================================\n");
    printf("ORBMDK_RDDI.dll Virtual Call Test\n");
    printf("========================================\n\n");

    // 待测 DLL：默认取"文件名"，LoadLibraryA 会先查 exe 所在目录
    // （build_test.ps1 把 exe 与 dll 都输出到 bin\）；也可用 argv[1] 显式指定。
    const char* dllPath = (argc > 1 && argv[1] && argv[1][0]) ? argv[1] : "ORBMDK_RDDI.dll";
    printf("Loading: %s\n\n", dllPath);

    // 加载 DLL
    HMODULE hDLL = LoadLibraryA(dllPath);
    if (!hDLL) {
        printf("FAIL: Cannot load DLL. Error: %d\n", GetLastError());
        return 1;
    }
    printf("OK: DLL loaded\n\n");

    // 获取函数指针
    #define GET_FUNC(name) \
        PFN_##name pfn_##name = (PFN_##name)GetProcAddress(hDLL, #name); \
        printf("  %-40s %s\n", #name, pfn_##name ? "OK" : "MISSING");

    printf("Functions:\n");
    GET_FUNC(RDDI_Open);
    GET_FUNC(RDDI_Close);
    GET_FUNC(RDDI_GetLastError);
    GET_FUNC(RDDI_SetLogCallback);
    GET_FUNC(CMSIS_DAP_Connect);
    GET_FUNC(CMSIS_DAP_ResetDAP);
    GET_FUNC(CMSIS_DAP_DetectNumberOfDevices);
    GET_FUNC(CMSIS_DAP_GetDeviceIDList);
    GET_FUNC(DAP_GetSupportedHostStatusIDs);
    GET_FUNC(CMSIS_DAP_ConfigureDebugger);
    GET_FUNC(DAP_ReadReg);
    printf("\n");

    if (!pfn_RDDI_Open) {
        printf("FAIL: Core functions missing\n");
        FreeLibrary(hDLL);
        return 1;
    }

    // 测试 Open
    printf("========================================\n");
    printf("Test 1: RDDI_Open\n");
    printf("========================================\n");

    RDDIHandle handle = 0;
    int result = pfn_RDDI_Open(&handle, NULL);
    printf("RDDI_Open: handle=%d, result=%d\n", handle, result);

    if (result == RDDI_SUCCESS) {
        printf("OK: RDDI_Open succeeded\n\n");

        // 设置日志回调
        pfn_RDDI_SetLogCallback(handle, LogCallback);

        // 测试 Connect
        printf("========================================\n");
        printf("Test 2: CMSIS_DAP_Connect\n");
        printf("========================================\n");

        int connectedInterface = 0;
        result = pfn_CMSIS_DAP_Connect(handle, &connectedInterface);
        printf("CMSIS_DAP_Connect: result=%d, interface=%d\n", result, connectedInterface);
        printf("  (%s)\n\n", connectedInterface == 1 ? "SWD" : "JTAG");

        // 测试设备检测
        printf("========================================\n");
        printf("Test 3: Device Detection\n");
        printf("========================================\n");

        int deviceCount = 0;
        result = pfn_CMSIS_DAP_DetectNumberOfDevices(handle, &deviceCount);
        printf("CMSIS_DAP_DetectNumberOfDevices: result=%d, count=%d\n", result, deviceCount);

        char deviceID[256] = {0};
        result = pfn_CMSIS_DAP_GetDeviceIDList(handle, &deviceCount, deviceID, sizeof(deviceID));
        printf("CMSIS_DAP_GetDeviceIDList: result=%d, deviceID='%s'\n\n", result, deviceID);

        // 测试状态 ID
        printf("========================================\n");
        printf("Test 4: Host Status IDs\n");
        printf("========================================\n");

        int statusCount = 0;
        int statusIDs[8] = {0};
        result = pfn_DAP_GetSupportedHostStatusIDs(handle, &statusCount, statusIDs);
        printf("DAP_GetSupportedHostStatusIDs: count=%d\n", statusCount);
        for (int i = 0; i < statusCount; i++) {
            printf("  statusIDs[%d] = 0x%02X\n", i, statusIDs[i]);
        }
        printf("\n");

        // 关闭
        printf("========================================\n");
        printf("Cleanup: RDDI_Close\n");
        printf("========================================\n");
        result = pfn_RDDI_Close(handle);
        printf("RDDI_Close: result=%d\n", result);

    } else {
        printf("FAIL: RDDI_Open failed\n");
    }

    FreeLibrary(hDLL);
    printf("\nDLL unloaded.\n");
    printf("\n========================================\n");
    printf("Test Complete!\n");
    printf("========================================\n");

    return 0;
}
