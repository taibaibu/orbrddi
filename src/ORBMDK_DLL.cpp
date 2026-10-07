/**
 * @file ORBMDK_DLL.cpp
 * @brief DLL 入口点和导出定义
 */

#include "pch.h"

#include "ORBMDK.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_HID.h"
#include "ORBMDK_USB_Bulk.h"

using namespace ORBMDK;

BOOL APIENTRY DllMain(HMODULE hModule,
                      DWORD   ul_reason_for_call,
                      LPVOID  lpReserved)
{
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            break;
        case DLL_PROCESS_DETACH:
            // ⚠️ 必须连同 **V2 Bulk 传输层**一起关（这里是它唯一的兜底关闭点）。
            //
            // AGDI 在两次 rddi_Open 之间会**卸载并重新加载**本 DLL：实测日志里
            // 重载后 `RDDI_Open: opening transport (current=0, ...)` —— 全局状态
            // 已归零，说明是一个新的 DLL 实例。
            //
            // 而 **WinUSB 句柄属于进程，不会随 DLL 卸载自动关闭**。这里只关 HID
            // 的话，MI_05 的设备句柄就永久泄漏在 UV4 进程里，重载后所有
            // CreateFile 都是 ERROR_ACCESS_DENIED(5) →
            // `Init: V2 Bulk unavailable -> fall back to V1 HID`。
            // 这正是"V2 只有第一次能打开、之后整场会话都退化成 V1"的根因。
            ORBMDK_USB_Bulk_Shutdown();
            ORBMDK_HID_Shutdown();
            break;
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
    }
    return TRUE;
}

extern "C" {

const char* ORBMDK_GetVersionString(void)
{
    return ORBMDK_VERSION_STRING;
}

void ORBMDK_GetVersion(int *major, int *minor, int *patch)
{
    if (major) *major = ORBMDK_VERSION_MAJOR;
    if (minor) *minor = ORBMDK_VERSION_MINOR;
    if (patch) *patch = ORBMDK_VERSION_PATCH;
}

} // extern "C"
