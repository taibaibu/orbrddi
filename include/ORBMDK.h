/**
 * @file ORBMDK.h
 * @brief ORBMDK - ORBTrace CMSIS-DAP AGDI 适配层
 *
 * ORBMDK provides AGDI (Application Generic Debug Interface) support for ORBTrace
 * debuggers using CMSIS-DAP protocol over USB HID.
 *
 * Architecture:
 *   Keil uVision (AGDI) -> ORBMDK_AGDI -> ORBMDK_RDDI -> USB HID -> ORBTrace
 */

#pragma once

// 导出 / 导入声明分离（不再依赖条件编译）
#define ORBMDK_EXPORT __declspec(dllexport)
#define ORBMDK_IMPORT __declspec(dllimport)

#ifdef ORBMDK_EXPORTS
#define ORBMDK_API      ORBMDK_EXPORT   // 构建 DLL：导出
#define ORBMDK_INTERNAL                 // 内部函数，不导出
#else
#define ORBMDK_API      ORBMDK_IMPORT   // 使用 DLL：导入
#define ORBMDK_INTERNAL                 // 静态库或直接包含时不导出
#endif

// Version info
#define ORBMDK_VERSION_MAJOR  1
#define ORBMDK_VERSION_MINOR  0
#define ORBMDK_VERSION_PATCH  0
#define ORBMDK_VERSION_STRING "1.0.0"

// ----------------------------------------------------------------------------
// ORBTrace USB 身份 (VID/PID)
//
// 必须与 orbtrace 固件保持一致，否则 V1(HID)/V2(Bulk) 会匹配到不同设备：
//   - orbtrace 固件 (orbtrace/debug/cmsis_test.py): VENDOR_ID=0x1209, PRODUCT_ID=0x3443
//   - orbuculum (Src/orbtraceIf.c):               { 0x1209, 0x3443, DEVICE_ORBTRACE_MINI }
//   - orbtrace README udev 规则:                  1209:3443 / 1209:3442
//
// HID 层仅按 VID 匹配（PID 由设备枚举阶段宽松处理），Bulk 层按 VID+PID 精确匹配，
// 因此两处必须使用同一份常量，禁止各自定义。
// ----------------------------------------------------------------------------
#define ORBMDK_ORBTRACE_VID   0x1209U
#define ORBMDK_ORBTRACE_PID   0x3443U
