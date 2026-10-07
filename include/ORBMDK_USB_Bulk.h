/**
 * @file ORBMDK_USB_Bulk.h
 * @brief USB Bulk 传输层 (CMSIS-DAP V2)
 *
 * 使用 WinUSB 或 libusb 实现 USB Bulk 传输
 */

#pragma once

#include "ORBMDK.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB Bulk V2 初始化状态
typedef enum {
    USB_BULK_NOT_INITED   = 0,
    USB_BULK_HID_MODE     = 1,    // V1 HID 模式
    USB_BULK_BULK_MODE    = 2,    // V2 Bulk 模式
} USB_Bulk_Mode;

// 连接信息
typedef struct {
    uint16_t vid;
    uint16_t pid;
    char serial[64];
    char product[128];
    uint8_t protocol_version;     // 1 = HID, 2 = Bulk
    uint32_t max_packet_size;
    uint32_t max_packet_count;
} USB_Bulk_Device_Info;

// ============================================================================
// USB Bulk API 函数
// ============================================================================

/**
 * @brief 初始化 USB Bulk 传输层
 * @param vid  Vendor ID
 * @param pid  Product ID
 * @param serial 设备序列号 (可选)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Init(uint16_t vid, uint16_t pid, const char* serial);

/**
 * @brief 关闭 USB Bulk 连接
 */
ORBMDK_INTERNAL void ORBMDK_USB_Bulk_Shutdown(void);

/**
 * @brief 检查是否已连接
 * @return 当前传输模式
 */
ORBMDK_INTERNAL USB_Bulk_Mode ORBMDK_USB_Bulk_GetMode(void);

/**
 * @brief 获取设备信息
 * @param info 输出设备信息
 * @return 0 成功
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetDeviceInfo(USB_Bulk_Device_Info* info);

/**
 * @brief 获取 V2 接口的产品名
 *
 * 来自 Bulk 接口自己的字符串描述符，通常为 "CMSIS-DAP v2"。
 * Keil 对话框里的适配器名字就是 CMSIS_DAP_Identify(idNo=2) 返回的产品名，
 * V2 模式下必须用这个而不是 HID 接口的产品名（否则显示 "CMSIS-DAP v1"）。
 *
 * @param buf 输出缓冲
 * @param len 缓冲长度
 * @return 0 成功（取到名字）, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_GetProductName(char* buf, size_t len);

/**
 * @brief 发送 DAP 命令并接收响应
 * @param cmd 命令数据
 * @param cmdLen 命令长度
 * @param resp 响应缓冲区
 * @param respLen 输入:缓冲区大小, 输出:实际响应长度
 * @param timeoutMs 超时时间(ms)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_DAPCommand(const uint8_t* cmd, size_t cmdLen,
                                uint8_t* resp, size_t* respLen, int timeoutMs);

/**
 * @brief 发送原始数据 (V2 Bulk 模式)
 * @param data 数据
 * @param len 数据长度
 * @param timeoutMs 超时
 * @return 发送的字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Write(const uint8_t* data, size_t len, int timeoutMs);

/**
 * @brief 接收原始数据 (V2 Bulk 模式)
 * @param data 接收缓冲区
 * @param maxLen 最大接收长度
 * @param timeoutMs 超时
 * @return 接收的字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_USB_Bulk_Read(uint8_t* data, size_t maxLen, int timeoutMs);

/**
 * @brief 获取 V2 协议能力
 * @return 能力标志
 */
ORBMDK_INTERNAL uint32_t ORBMDK_USB_Bulk_GetCapabilities(void);

/**
 * @brief 设置 V2 协议超时
 * @param timeoutMs 超时时间(ms)
 */
ORBMDK_INTERNAL void ORBMDK_USB_Bulk_SetTimeout(int timeoutMs);

// ============================================================================
// CMSIS-DAP V2 专用 API
// ============================================================================

/**
 * @brief 获取 V2 协议信息
 * @param info_id 信息 ID
 * @param buffer 输出缓冲区
 * @param buffer_len 缓冲区长度
 * @return >0 成功 (返回长度), <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_GetInfo(uint8_t info_id, uint8_t* buffer, size_t buffer_len);

/**
 * @brief 启动 V2 时间戳
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStart(void);

/**
 * @brief 读取 V2 时间戳
 * @param timestamp 时间戳输出
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerRead(uint32_t* timestamp);

/**
 * @brief 停止 V2 时间戳
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_TimerStop(void);

/**
 * @brief 设置 V2 超时
 * @param timeout_ms 超时(ms)
 * @return 0 成功
 */
ORBMDK_INTERNAL int CMSIS_DAP_V2_SetTimeout(int timeout_ms);

/**
 * @brief 获取 V2 协议版本
 * @return 协议版本 (1=HID, 2=Bulk)
 */
ORBMDK_INTERNAL uint8_t CMSIS_DAP_V2_GetProtocolVersion(void);

/**
 * @brief 获取 V2 能力标志
 * @return 能力位掩码
 */
ORBMDK_INTERNAL uint32_t CMSIS_DAP_V2_GetCapabilities(void);

// ============================================================================
// USB Bulk 直接读写 (V2 模式)
// ============================================================================

/**
 * @brief 打开 USB Bulk 设备
 * @param vid Vendor ID
 * @param pid Product ID
 * @param serial 序列号 (可选)
 * @return 0 成功, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Open(uint16_t vid, uint16_t pid, const char* serial);

/**
 * @brief 关闭 USB Bulk 设备
 */
ORBMDK_INTERNAL void ORBMDK_Bulk_Close(void);

/**
 * @brief USB Bulk 写入
 * @param data 数据
 * @param len 长度
 * @param timeout_ms 超时
 * @return 写入字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Write(const uint8_t* data, size_t len, int timeout_ms);

/**
 * @brief USB Bulk 读取
 * @param data 接收缓冲区
 * @param max_len 最大长度
 * @param timeout_ms 超时
 * @return 读取字节数, <0 失败
 */
ORBMDK_INTERNAL int ORBMDK_Bulk_Read(uint8_t* data, size_t max_len, int timeout_ms);

#ifdef __cplusplus
}
#endif
