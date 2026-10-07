/**
 * @file pch.h
 * @brief 预编译头文件
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

// 抑制 C++17 弃用警告
#define _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS

// SDK 兼容性定义
#define UMDF_USING_IOCTL_DEFINE_GUIDS
#define COBJMACROS
#define INITGUID

#include <Windows.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <memory>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <list>
#include <queue>
#include <deque>
#include <array>
#include <tuple>
#include <utility>
#include <functional>
#include <algorithm>
#include <numeric>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <type_traits>
#include <initializer_list>

#include "../include/ORBMDK.h"
#include "../include/ORBMDK_DAP.h"
#include "../include/ORBMDK_RDDI.h"
#include "../include/ORBMDK_HID.h"
// deprecated/ORBMDK_AGDI.h         # AGDI 层由 elaphureLinkAGDI 提供
// deprecated/ORBMDK_CMSIS_COMPAT.h # 不在 pch.h 中包含，避免与 ORBMDK_RDDI.h 冲突
#include "../include/ORBMDK_DAPV2.h"
#include "../include/ORBMDK_USB_Bulk.h"
#include "../include/ORBMDK_Trace.h"
#include "../include/ORBMDK_COBS.h"
#include "../include/ORBMDK_OFLOW.h"
// deprecated/ORBMDK_AGDI_Trace.h   # AGDI Trace 由 elaphureLinkAGDI 提供
