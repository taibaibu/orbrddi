/**
 * @file ORBMDK_Symbols.h
 * @brief 符号解析接口
 *
 * 支持两种模式:
 * 1. Objdump 模式: 调用 arm-none-eabi-objdump 解析符号
 * 2. DWARF 模式: 直接使用 libdwarf/libelf 解析 DWARF 信息
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// 错误码
// ============================================================================

enum ORBMDK_SymbolErr {
    ORBMDK_SYMBOL_OK = 0,
    ORBMDK_SYMBOL_ERROR = -1,
    ORBMDK_SYMBOL_NOFILE = -2,
    ORBMDK_SYMBOL_NOMEM = -3,
    ORBMDK_SYMBOL_INVALID = -4,
    ORBMDK_SYMBOL_NOTFOUND = -5,
};

// ============================================================================
// 文件条目
// ============================================================================

struct ORBMDK_SymbolFile {
    const char* name;     // 文件路径
    uint32_t index;       // 文件索引
};

// ============================================================================
// 函数条目
// ============================================================================

struct ORBMDK_SymbolFunc {
    const char* name;             // 函数名
    uint32_t startAddr;          // 起始地址
    uint32_t endAddr;            // 结束地址
    uint32_t fileIndex;          // 所属文件索引
};

// ============================================================================
// 源码行条目
// ============================================================================

struct ORBMDK_SymbolLine {
    uint32_t startAddr;          // 起始地址
    uint32_t endAddr;            // 结束地址
    uint32_t lineNo;             // 源文件行号
    uint32_t functionIndex;      // 所属函数索引
    uint32_t fileIndex;          // 所属文件索引
};

// ============================================================================
// 名字查找结果
// ============================================================================

struct ORBMDK_SymbolLookupResult {
    const char* fileName;        // 文件名
    const char* functionName;    // 函数名
    uint32_t lineNo;             // 行号
    uint32_t functionIndex;      // 函数索引
    uint32_t fileIndex;          // 文件索引
    uint32_t address;            // 地址
};

// ============================================================================
// 符号集合
// ============================================================================

struct ORBMDK_SymbolSet;

// ============================================================================
// 创建/销毁
// ============================================================================

/**
 * @brief 创建符号集合 (Objdump 模式)
 *
 * @param filename ELF 文件路径
 * @param deleteMaterial 删除的文件名前缀 (可为 NULL)
 * @param demangleCpp 是否对 C++ 名称解 mangling
 * @param recordSource 是否记录源码
 * @param objdumpPath objdump 工具路径 (可为 NULL 使用默认)
 * @return 符号集句柄，失败返回 NULL
 */
struct ORBMDK_SymbolSet* ORBMDK_SymbolSet_Create(
    const char* filename,
    const char* deleteMaterial,
    bool demangleCpp,
    bool recordSource,
    const char* objdumpPath);

/**
 * @brief 创建符号集合 (DWARF 模式，使用 libdwarf)
 *
 * @param filename ELF 文件路径
 * @param loadMem 是否加载内存段
 * @param loadSource 是否加载源码
 * @return 符号集句柄，失败返回 NULL
 */
struct ORBMDK_SymbolSet* ORBMDK_SymbolSet_CreateDWARF(
    const char* filename,
    bool loadMem,
    bool loadSource);

/**
 * @brief 销毁符号集合
 * @param ss 符号集句柄
 */
void ORBMDK_SymbolSet_Destroy(struct ORBMDK_SymbolSet* ss);

// ============================================================================
// 查询
// ============================================================================

/**
 * @brief 检查符号集是否有效
 * @param ss 符号集句柄
 * @return true 有效，false 无效
 */
bool ORBMDK_SymbolSet_Valid(struct ORBMDK_SymbolSet* ss);

/**
 * @brief 根据地址查找符号信息
 *
 * @param ss 符号集句柄
 * @param addr 要查询的地址
 * @param result 结果结构体指针
 * @return true 找到，false 未找到
 */
bool ORBMDK_Symbol_Lookup(struct ORBMDK_SymbolSet* ss, uint32_t addr,
    struct ORBMDK_SymbolLookupResult* result);

/**
 * @brief 获取文件名
 * @param ss 符号集句柄
 * @param index 文件索引
 * @return 文件名，失败返回 NULL
 */
const char* ORBMDK_Symbol_Filename(struct ORBMDK_SymbolSet* ss, uint32_t index);

/**
 * @brief 获取函数名
 * @param ss 符号集句柄
 * @param index 函数索引
 * @return 函数名，失败返回 NULL
 */
const char* ORBMDK_Symbol_Function(struct ORBMDK_SymbolSet* ss, uint32_t index);

/**
 * @brief 获取指定地址的函数信息
 * @param ss 符号集句柄
 * @param addr 地址
 * @return 函数信息，失败返回 NULL
 */
const struct ORBMDK_SymbolFunc* ORBMDK_Symbol_FunctionAt(struct ORBMDK_SymbolSet* ss, uint32_t addr);

/**
 * @brief 获取指定地址的源码行信息
 * @param ss 符号集句柄
 * @param addr 地址
 * @return 源码行信息，失败返回 NULL
 */
const struct ORBMDK_SymbolLine* ORBMDK_Symbol_LineAt(struct ORBMDK_SymbolSet* ss, uint32_t addr);

/**
 * @brief 获取地址对应的汇编指令文本
 * @param ss 符号集句柄
 * @param addr 地址
 * @param buffer 输出缓冲区
 * @param bufferSize 缓冲区大小
 * @return 实际写入的字节数
 */
size_t ORBMDK_Symbol_Disassemble(struct ORBMDK_SymbolSet* ss, uint32_t addr,
    char* buffer, size_t bufferSize);

// ============================================================================
// 统计
// ============================================================================

/**
 * @brief 获取文件数量
 * @param ss 符号集句柄
 * @return 文件数量
 */
uint32_t ORBMDK_SymbolSet_GetFileCount(struct ORBMDK_SymbolSet* ss);

/**
 * @brief 获取函数数量
 * @param ss 符号集句柄
 * @return 函数数量
 */
uint32_t ORBMDK_SymbolSet_GetFunctionCount(struct ORBMDK_SymbolSet* ss);

/**
 * @brief 获取源码行数量
 * @param ss 符号集句柄
 * @return 源码行数量
 */
uint32_t ORBMDK_SymbolSet_GetLineCount(struct ORBMDK_SymbolSet* ss);

/**
 * @brief 迭代所有文件
 * @param ss 符号集句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_SymbolSet_IterateFiles(struct ORBMDK_SymbolSet* ss,
    void (*callback)(const struct ORBMDK_SymbolFile* file, void* userData),
    void* userData);

/**
 * @brief 迭代所有函数
 * @param ss 符号集句柄
 * @param callback 回调函数
 * @param userData 用户数据
 */
void ORBMDK_SymbolSet_IterateFunctions(struct ORBMDK_SymbolSet* ss,
    void (*callback)(const struct ORBMDK_SymbolFunc* func, void* userData),
    void* userData);

// ============================================================================
// 工具路径检测
// ============================================================================

/**
 * @brief 检测 objdump 工具路径
 *
 * 搜索顺序:
 * 1. 环境变量 OBJDUMP 指定的路径
 * 2. 环境变量 ARMGCC_DIR 或 ARM_TOOLCHAIN_PATH 下的 bin 目录
 * 3. Keil 安装目录 (C:\Keil_v5\ARM\ARMCC\Bin)
 * 4. PATH 环境变量中搜索 arm-none-eabi-objdump
 * 5. 默认路径 arm-none-eabi-objdump
 *
 * @param buffer 输出缓冲区
 * @param bufferSize 缓冲区大小
 * @return true 找到，false 未找到
 */
bool ORBMDK_FindObjdumpPath(char* buffer, size_t bufferSize);

#ifdef __cplusplus
}
#endif
