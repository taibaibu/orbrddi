/**
 * @file ORBMDK_DAP.h
 * @brief CMSIS-DAP 协议定义
 *
 * 基于 CMSIS-DAP 规范定义的协议命令和响应格式
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// CMSIS-DAP Command IDs
// ============================================================================
enum ORBMDK_DAP_Command {
    ID_DAP_INFO              = 0x00U,  // Get DAP info
    ID_DAP_HOST_STATUS       = 0x01U,  // Host status
    ID_DAP_CONNECT           = 0x02U,  // Connect to target
    ID_DAP_DISCONNECT        = 0x03U,  // Disconnect from target
    ID_DAP_TRANSFER_CONFIG   = 0x04U,  // Configure transfer
    ID_DAP_TRANSFER          = 0x05U,  // Single transfer
    ID_DAP_TRANSFER_BLOCK    = 0x06U,  // Block transfer
    ID_DAP_TRANSFER_ABORT    = 0x07U,  // Abort transfer
    ID_DAP_WRITE_ABORT       = 0x08U,  // Write ABORT register
    ID_DAP_DELAY             = 0x09U,  // Delay execution
    ID_DAP_RESET_TARGET      = 0x0AU,  // Reset target
    ID_DAP_SWJ_PINS          = 0x10U,  // SWJ Pin control
    ID_DAP_SWJ_CLOCK         = 0x11U,  // SWJ Clock set
    ID_DAP_SWJ_SEQUENCE      = 0x12U,  // SWJ Sequence
    ID_DAP_SWD_CONFIGURE     = 0x13U,  // SWD configure
    ID_DAP_JTAG_SEQUENCE      = 0x14U,  // JTAG Sequence
    ID_DAP_JTAG_CONFIGURE     = 0x15U,  // JTAG Configure
    ID_DAP_JTAG_IDCODE        = 0x16U,  // JTAG IDCODE
    ID_DAP_SWO_TRANSPORT      = 0x17U,  // SWO Transport
    ID_DAP_SWO_MODE           = 0x18U,  // SWO Mode
    ID_DAP_SWO_BAUDRATE       = 0x19U,  // SWO Baudrate
    ID_DAP_SWO_CONTROL        = 0x1AU,  // SWO Control
    ID_DAP_SWO_STATUS         = 0x1BU,  // SWO Status
    ID_DAP_SWO_DATA           = 0x1CU,  // SWO Data
    ID_DAP_SWO_EXTENDED_STATUS= 0x1EU,  // SWO Extended Status
    ID_DAP_SWD_SEQUENCE       = 0x1DU,  // SWD Sequence
    ID_DAP_QUEUE_COMMANDS     = 0x7EU,  // Queue commands
    ID_DAP_EXECUTE_COMMANDS   = 0x7FU,  // Execute commands
};

// ============================================================================
// DAP Response Status
// ============================================================================
enum ORBMDK_DAP_Response {
    DAP_RES_OK              = 0,   // Transfer OK (CMSIS-DAP: 0=ACK OK)
    DAP_RES_WAIT            = 1,   // Transfer WAIT (CMSIS-DAP: 1=ACK WAIT)
    DAP_RES_FAULT           = 2,   // Transfer FAULT (CMSIS-DAP: 2=ACK FAULT)
    DAP_RES_NO_ACK          = 3,   // No ACK from target (reserved in spec)
    DAP_RES_VALUE_MISMATCH  = 16,  // Value mismatch
    DAP_RES_ERROR           = 0xFF // General error
};

// ============================================================================
// DAP Info IDs (for DAP_INFO command)
// ============================================================================
enum ORBMDK_DAP_Info {
    DAP_INFO_VENDOR          = 1,
    DAP_INFO_PRODUCT         = 2,
    DAP_INFO_SERIAL          = 3,
    DAP_INFO_FIRMWARE        = 4,
    DAP_INFO_CAPS            = 5,     // Capabilities
    DAP_INFO_PACKET_COUNT    = 6,     // Max packet count
    DAP_INFO_PACKET_SIZE     = 7,     // Max packet size
    DAP_INFO_SWO_TRACE_BUF   = 8,     // SWO trace buffer size
    DAP_INFO_SWO_TRACE_CNT   = 9,     // SWO trace count
    DAP_INFO_SWO_FLUSH_CNT   = 10,    // SWO flush count
    DAP_INFO_CAP_COUNT       = 11,    // Capability count
};

// ============================================================================
// DAP Capabilities
// ============================================================================
enum ORBMDK_DAP_Capabilities {
    DAP_CAP_SWD             = (1U << 0),  // SWD supported
    DAP_CAP_JTAG            = (1U << 1),  // JTAG supported
    DAP_CAP_SWO_UART        = (1U << 2),  // SWO UART mode
    DAP_CAP_SWO_MANCHESTER  = (1U << 3),  // SWO Manchester mode
    DAP_CAP_SWO_SERIAL      = (1U << 4),  // SWO Serial mode
    DAP_CAP_SWO_MASK        = (0x1FU << 2),
    DAP_CAP_ATOMIC          = (1U << 8),  // Atomic commands
    DAP_CAP_DAP_SWO         = (1U << 16), // DAP SWO trace
    DAP_CAP_DAP_DBG        = (1U << 17), // DAP debug
};

// ============================================================================
// Connect Mode
// ============================================================================
enum ORBMDK_DAP_Connect_Mode {
    DAP_CONNECT_DEFAULT     = 0,
    DAP_CONNECT_SWD         = 1,
    DAP_CONNECT_JTAG        = 2,
};

// ============================================================================
// SWJ Pin definitions
// ============================================================================
enum ORBMDK_DAP_Pin {
    DAP_PIN_SWCLK           = (1U << 0),  // SWCLK/TCK
    DAP_PIN_SWDIO          = (1U << 1),  // SWDIO/TMS
    DAP_PIN_TDI             = (1U << 2),  // TDI
    DAP_PIN_TDO             = (1U << 3),  // TDO
    DAP_PIN_nTRST           = (1U << 5),  // nTRST
    DAP_PIN_nRESET          = (1U << 7),  // nRESET
};

// ============================================================================
// ADI v5 Register Offsets (Debug Port)
// ============================================================================
enum ORBMDK_DP_Registers {
    DP_REG_IDCODE           = 0x00U,  // IDCODE (read-only)
    DP_REG_ABORT            = 0x00U,  // ABORT (write-only)
    DP_REG_CTRL_STAT        = 0x04U,  // CTRL/STAT
    DP_REG_WCR              = 0x04U,  // Wire Control Register
    DP_REG_RESEND           = 0x08U,  // RESEND
    DP_REG_SELECT           = 0x08U,  // SELECT
    DP_REG_RDBUFF           = 0x0CU,  // RDBUFF
    DP_REG_TARGETID         = 0x0CU,  // Target Identification
    DP_REG_DLPIDR           = 0x12U,  // DLPIDR (DAP-Link)
};

// ============================================================================
// ADI v5 Register Offsets (Access Port)
// ============================================================================
enum ORBMDK_AP_Registers {
    AP_REG_CSW              = 0x00U,  // CSW
    AP_REG_TAR              = 0x04U,  // TAR
    AP_REG_DRW              = 0x0CU,  // DRW
    AP_REG_DB0              = 0x10U,  // Data Bank 0
    AP_REG_DB1              = 0x14U,  // Data Bank 1
    AP_REG_DB2              = 0x18U,  // Data Bank 2
    AP_REG_DB3              = 0x1CU,  // Data Bank 3
    AP_REG_BASE             = 0xF8U,  // BASE
    AP_REG_CFG              = 0xF4U,  // CFG
    AP_REG_EXT              = 0xF0U,  // EXT
    AP_REG_IDR              = 0xFCU,  // IDR
};

// ============================================================================
// DP Control/Status Register bits
// ============================================================================
enum ORBMDK_DP_CTRL_STAT {
    CS_ORUNDETECT           = (1U << 0),  // Overrun detect
    CS_STICKYERR            = (1U << 5),  // Sticky error
    CS_STICKYCMP            = (1U << 4),  // Sticky compare
    CS_TOUTPRE              = (1U << 3),  // Timeout prescalar
    CS_STICKYORUN           = (1U << 2),  // Sticky overrun
    CS_WDATAERR             = (1U << 1),  // Write data error
    CS_MASKLANE             = (1U << 16), // Mask lane
    CS_TRNMODE              = (1U << 20), // Transfer mode
    CS_STRESPCLR            = (1U << 31), // Status response clear
};

// ============================================================================
// AP Control/Status Word bits
// ============================================================================
enum ORBMDK_AP_CSW {
    CSW_DBGSTATUS           = (1U << 0),  // Debug status
    CSW_SPIDEN              = (1U << 1),  // SPI debug enable
    CSW_SPROT               = (1U << 2),  // Secure protected
    CSW_WDATAERR            = (1U << 3),  // Write data error
    CSW_ORUNDETECT          = (1U << 4),  // Overrun detect
    CSW_MASKLANE            = (1U << 6),  // Mask lane
    CSW_EXTRESPCLR          = (1U << 7),  // External response clear
    // Address increment
    CSW_ADDRINC_NONE        = (0U << 4),
    CSW_ADDRINC_SINGLE      = (1U << 4),
    CSW_ADDRINC_PACKED      = (2U << 4),
    // Device type
    CSW_DEV_NONE            = (0U << 24),
    CSW_DEV_AHB             = (1U << 24),
    CSW_DEV_APB             = (2U << 24),
    // Size
    CSW_SIZE_BYTE           = (0U << 0),
    CSW_SIZE_HALF           = (1U << 0),
    CSW_SIZE_WORD           = (2U << 0),
};

// ============================================================================
// AP Abort Register bits
// ============================================================================
enum ORBMDK_DAP_ABORT {
    ABORT_DAPABORT          = (1U << 0),  // DAP abort
    ABORT_STKERRCLR         = (1U << 1),  // Clear sticky error
    ABORT_WDERRCLR          = (1U << 2),  // Clear write data error
    ABORT_ORUNERRCLR         = (1U << 4),  // Clear overrun error
};

// ============================================================================
// Transfer Request bits
// ============================================================================
enum ORBMDK_Transfer_Request {
    TR_APNDP                = (1U << 0),   // AP(1)/DP(0)
    TR_RNW                  = (1U << 1),   // Read(1)/Write(0)
    TR_A3                  = (1U << 2),   // Address bit 3
    TR_A2                  = (1U << 3),   // Address bit 2
    TR_VALUE_MATCH          = (1U << 4),   // Value match
    TR_MATCH_MASK           = (1U << 5),   // Match mask
};

#ifdef __cplusplus
}
#endif
