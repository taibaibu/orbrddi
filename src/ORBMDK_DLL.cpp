/**
 * @file ORBMDK_DLL.cpp
 * @brief DLL 入口点和导出定义
 */

#include "pch.h"

#include "ORBMDK.h"
#include "ORBMDK_RDDI.h"
#include "ORBMDK_HID.h"

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
