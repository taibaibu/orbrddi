/**
 * @file ORBMDK_Symbols.cpp
 * @brief 符号解析实现
 *
 * 支持两种模式:
 * 1. Objdump 模式: 调用 arm-none-eabi-objdump 解析符号
 * 2. DWARF 模式: 直接使用 libdwarf/libelf 解析 DWARF 信息
 *
 * 参考: orbuculum Src/symbols.c 和 Src/loadelf.c
 */

#include "pch.h"
#include "ORBMDK_Symbols.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <algorithm>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

// ============================================================================
// MSVC 兼容: 字节序交换 (GCC bswap 在 MSVC 不可用)
// ============================================================================
static inline uint16_t bswap16(uint16_t x) {
    return ((x & 0x00FF) << 8) | ((x & 0xFF00) >> 8);
}

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0x000000FF) << 24) |
           ((x & 0x0000FF00) << 8)  |
           ((x & 0x00FF0000) >> 8)  |
           ((x & 0xFF000000) >> 24);
}

// ============================================================================
// 常量定义
// ============================================================================

#define MAX_LINE_LEN 4096
#define MAX_ADDR_RANGES 1024

// Objdump 默认路径
#ifndef OBJDUMP_PATH
#ifdef _WIN32
#define OBJDUMP_PATH "arm-none-eabi-objdump"
#else
#define OBJDUMP_PATH "arm-none-eabi-objdump"
#endif
#endif

// ELF 符号类型 (简化定义，避免依赖 elf.h)
#define STT_NOTYPE   0
#define STT_OBJECT   1
#define STT_FUNC     2
#define STT_SECTION  3
#define STT_FILE     4
#define STT_COMMON   5
#define STT_TLS      6

// Objdump 输出中的源码标记
#define SOURCE_INDICATOR "Src##"

// ============================================================================
// 行类型 (解析 objdump 输出)
// ============================================================================

enum LineType {
    LT_NULL,
    LT_NOISE,
    LT_PROC_LABEL,
    LT_LABEL,
    LT_SOURCE,
    LT_ASSEMBLY,
    LT_FILEANDLINE,
    LT_NEWLINE,
    LT_ERROR
};

// ============================================================================
// 汇编行条目
// ============================================================================

struct AssyLineEntry {
    uint32_t addr;
    uint32_t lineNo;
    bool isJump;
    bool isSubCall;
    bool isReturn;
    bool is4Byte;
    uint32_t codes;
    char text[256];
};

// ============================================================================
// 内部数据结构
// ============================================================================

// 内部结构使用 std::string 存储，公共 API 需要返回 POD 类型
struct ORBMDK_SymbolFileInt {
    std::string name;
    uint32_t index;
};

struct ORBMDK_SymbolFuncInt {
    std::string name;
    uint32_t startAddr;
    uint32_t endAddr;
    uint32_t fileIndex;
};

struct ORBMDK_SymbolLineInt {
    uint32_t startAddr;
    uint32_t endAddr;
    uint32_t lineNo;
    uint32_t functionIndex;
    uint32_t fileIndex;
    std::string lineText;
    std::vector<AssyLineEntry> assyLines;
};

// 公共 API 使用的临时变量 (避免栈分配)
static thread_local struct ORBMDK_SymbolFunc g_funcResult = {};
static thread_local struct ORBMDK_SymbolLine g_lineResult = {};
static thread_local char g_funcNameBuf[256] = {};
static thread_local char g_lineTextBuf[512] = {};

struct ORBMDK_SymbolSetInt {
    // 文件信息
    std::vector<ORBMDK_SymbolFileInt> files;
    std::vector<ORBMDK_SymbolFuncInt> functions;
    std::vector<ORBMDK_SymbolLineInt> lines;

    // 配置
    std::string elfFile;
    std::string deleteMaterial;
    std::string objdumpOptions;
    bool recordSource;
    bool recordAssy;
    bool demangleCpp;
    bool useObjdump;  // true=Objdump 模式, false=DWARF 模式

    // Objdump 进程
    FILE* objdumpPipe;
    char lastFile[MAX_LINE_LEN];
    uint32_t lastLine;

    // 缓存
    uint32_t cachedSearchIndex;
};

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief 简单 C++ 名称 demangle
 */
static std::string demangleCpp(const char* name)
{
    // 简化实现，只处理常见的模式
    std::string s(name);

    // _Z[n]name -> name 简化处理
    if (s.substr(0, 3) == "_Z") {
        size_t len = 0;
        if (s.length() > 3 && isdigit(s[3])) {
            size_t i = 3;
            while (i < s.length() && isdigit(s[i])) {
                len = len * 10 + (s[i] - '0');
                i++;
            }
            if (len > 0 && i + len <= s.length()) {
                return s.substr(i, len);
            }
        }
    }

    // _ZN -> _ZN (未完成)
    // 等等...

    return s;
}

/**
 * @brief 去除路径前缀
 */
static void trimFilename(char* name, const char* deleteMaterial)
{
    if (!name || !deleteMaterial) return;

    const char* p = strstr(name, deleteMaterial);
    if (p) {
        size_t offset = p - name + strlen(deleteMaterial);
        memmove(name, name + offset, strlen(name + offset) + 1);
    }
}

/**
 * @brief 解析汇编指令
 */
static bool parseAssemblyLine(const char* line, AssyLineEntry* entry)
{
    uint32_t addr;
    char opcode[64];
    char mnemonic[32];

    // 格式: "   0x08001234 <+0>:   ldr r0, [r7, #4]"
    int matched = sscanf(line, " %*s 0x%x <+%*d>: %s %[^\n]",
        &addr, opcode, mnemonic);

    if (matched >= 2) {
        entry->addr = addr;
        entry->codes = 0;

        // 解析操作码
        if (strcmp(opcode, "bl") == 0 || strcmp(opcode, "blx") == 0) {
            entry->isSubCall = true;
        } else if (strcmp(opcode, "bx") == 0 || strcmp(opcode, "pop") == 0) {
            entry->isReturn = true;
        } else if (strstr(mnemonic, "b.") || strcmp(opcode, "b") == 0) {
            entry->isJump = true;
        }

        strncpy(entry->text, line, sizeof(entry->text) - 1);
        entry->text[sizeof(entry->text) - 1] = '\0';
        return true;
    }

    return false;
}

/**
 * @brief 解析行类型
 */
static LineType getLineType(const char* line)
{
    if (!line || line[0] == '\0') return LT_NEWLINE;
    if (strstr(line, "file format") != nullptr) return LT_NOISE;
    if (strstr(line, "Disassembly") != nullptr) return LT_NOISE;

    // 函数标签: "00000000 <main>:"
    if (strchr(line, '<') != nullptr && strchr(line, '>') != nullptr) {
        return LT_PROC_LABEL;
    }

    // 文件和行: "c:/path/file.c:10"
    if (strstr(line, ".c:") != nullptr || strstr(line, ".h:") != nullptr ||
        strstr(line, ".C:") != nullptr || strstr(line, ".s:") != nullptr) {
        return LT_FILEANDLINE;
    }

    // 源码行
    if (strstr(line, SOURCE_INDICATOR) != nullptr) {
        return LT_SOURCE;
    }

    // 汇编行
    if (strstr(line, "0x") != nullptr && strchr(line, ':') != nullptr) {
        return LT_ASSEMBLY;
    }

    // 标签: "  bLabel:"
    if (line[0] == ' ' && isalpha(line[1]) && strchr(line, ':') != nullptr) {
        return LT_LABEL;
    }

    return LT_NOISE;
}

/**
 * @brief 从函数标签提取函数名
 */
static std::string extractFunctionName(const char* line)
{
    const char* start = strchr(line, '<');
    const char* end = strchr(line, '>');
    if (start && end && end > start + 1) {
        return std::string(start + 1, end - start - 1);
    }
    return "";
}

// ============================================================================
// Objdump 模式实现
// ============================================================================

/**
 * @brief 创建 Objdump 模式的符号集
 */
struct ORBMDK_SymbolSet* ORBMDK_SymbolSet_Create(
    const char* filename,
    const char* deleteMaterial,
    bool demangleCpp,
    bool recordSource,
    const char* objdumpPath)
{
    if (!filename) return nullptr;

    struct ORBMDK_SymbolSetInt* ss = new struct ORBMDK_SymbolSetInt;
    if (!ss) return nullptr;

    memset(ss, 0, sizeof(*ss));
    ss->elfFile = filename;
    ss->recordSource = recordSource;
    ss->recordAssy = true;
    ss->demangleCpp = demangleCpp;
    ss->useObjdump = true;
    ss->objdumpPipe = nullptr;
    ss->cachedSearchIndex = 0;

    if (deleteMaterial) {
        ss->deleteMaterial = deleteMaterial;
    }

    // 构建 objdump 命令
    char commandLine[MAX_LINE_LEN];
    const char* objdump = objdumpPath ? objdumpPath : OBJDUMP_PATH;

    snprintf(commandLine, sizeof(commandLine),
        "\"%s\" -Sl%s --source-comment=" SOURCE_INDICATOR " \"%s\" %s",
        objdump,
        demangleCpp ? " -C" : "",
        filename,
        ss->objdumpOptions.c_str());

    // 打开管道读取 objdump 输出
    ss->objdumpPipe = _popen(commandLine, "r");
    if (!ss->objdumpPipe) {
        delete ss;
        return nullptr;
    }

    return (struct ORBMDK_SymbolSet*)ss;
}

/**
 * @brief 读取下一行 objdump 输出
 */
static bool readObjdumpLine(FILE* pipe, char* buffer, size_t size)
{
    if (!fgets(buffer, static_cast<int>(size), pipe)) {
        return false;
    }

    // 去除换行符
    size_t len = strlen(buffer);
    if (len > 0 && buffer[len - 1] == '\n') {
        buffer[len - 1] = '\0';
    }

    return true;
}

/**
 * @brief 解析源码标记行
 */
static void parseSourceComment(const char* line, char* filename, size_t fnameSize, uint32_t* lineNo)
{
    const char* marker = strstr(line, SOURCE_INDICATOR);
    if (!marker) return;

    marker += strlen(SOURCE_INDICATOR);

    // 格式: "filename:line"
    const char* colon = strrchr(marker, ':');
    if (colon) {
        size_t fnameLen = colon - marker;
        if (fnameLen < fnameSize) {
            strncpy(filename, marker, fnameLen);
            filename[fnameLen] = '\0';
            *lineNo = atoi(colon + 1);
        }
    }
}

// ============================================================================
// DWARF 模式实现 (简化版)
// ============================================================================

/**
 * @brief 创建 DWARF 模式的符号集
 */
struct ORBMDK_SymbolSet* ORBMDK_SymbolSet_CreateDWARF(
    const char* filename,
    bool loadMem,
    bool loadSource)
{
    if (!filename) return nullptr;

    struct ORBMDK_SymbolSetInt* ss = new struct ORBMDK_SymbolSetInt;
    if (!ss) return nullptr;

    memset(ss, 0, sizeof(*ss));
    ss->elfFile = filename;
    ss->recordSource = loadSource;
    ss->recordAssy = false;
    ss->demangleCpp = false;
    ss->useObjdump = false;
    ss->objdumpPipe = nullptr;
    ss->cachedSearchIndex = 0;

    // 简化实现: 使用 objdump -g 来获取 DWARF 信息
    // 完整实现需要 libdwarf/libelf

    // 尝试解析 ELF 文件
    FILE* f = fopen(filename, "rb");
    if (!f) {
        delete ss;
        return nullptr;
    }

    // 读取 ELF 头
    unsigned char ehdr[64];
    if (fread(ehdr, 1, sizeof(ehdr), f) != sizeof(ehdr)) {
        fclose(f);
        delete ss;
        return nullptr;
    }

    // 检查 ELF Magic
    if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') {
        fclose(f);
        delete ss;
        return nullptr;
    }

    // 获取 Section Header Table 偏移
    bool isLittleEndian = (ehdr[5] == 1);
    uint32_t shoff = isLittleEndian ?
        *(uint32_t*)(ehdr + 40) : 
        bswap32(*(uint32_t*)(ehdr + 40));
    uint16_t shentsize = isLittleEndian ?
        *(uint16_t*)(ehdr + 46) :
        bswap16(*(uint16_t*)(ehdr + 46));
    uint16_t shnum = isLittleEndian ?
        *(uint16_t*)(ehdr + 48) :
        bswap16(*(uint16_t*)(ehdr + 48));
    uint16_t shstrndx = isLittleEndian ?
        *(uint16_t*)(ehdr + 50) :
        bswap16(*(uint16_t*)(ehdr + 50));

    // 读取 Section Header 字符串表
    fseek(f, shoff + shstrndx * shentsize, SEEK_SET);
    unsigned char shStrSec[16];
    fread(shStrSec, 1, sizeof(shStrSec), f);

    uint32_t strTabOff = isLittleEndian ?
        *(uint32_t*)(shStrSec + 24) :
        bswap32(*(uint32_t*)(shStrSec + 24));
    uint32_t strTabSize = isLittleEndian ?
        *(uint32_t*)(shStrSec + 28) :
        bswap32(*(uint32_t*)(shStrSec + 28));

    std::vector<char> strTab(strTabSize + 1);
    fseek(f, strTabOff, SEEK_SET);
    fread(strTab.data(), 1, strTabSize, f);
    strTab[strTabSize] = '\0';

    // 读取所有 Section Headers
    struct {
        uint32_t name;
        uint32_t type;
        uint32_t flags;
        uint32_t addr;
        uint32_t offset;
        uint32_t size;
        uint32_t link;
        uint32_t info;
        uint32_t addralign;
        uint32_t entsize;
    } shdr;

    std::vector<std::string> sectionNames;

    for (uint16_t i = 0; i < shnum; i++) {
        fseek(f, shoff + i * shentsize, SEEK_SET);
        fread(&shdr, 1, sizeof(shdr), f);

        if (isLittleEndian) {
            // 数据已经是小端
        } else {
            shdr.name = bswap32(shdr.name);
            shdr.type = bswap32(shdr.type);
            shdr.offset = bswap32(shdr.offset);
            shdr.size = bswap32(shdr.size);
        }

        std::string secName = &strTab[shdr.name];
        sectionNames.push_back(secName);

        // 解析 .symtab 和 .strtab
        if (secName == ".symtab") {
            // 读取符号表
            std::vector<unsigned char> symData(shdr.size);
            fseek(f, shdr.offset, SEEK_SET);
            fread(symData.data(), 1, shdr.size, f);

            uint32_t symCount = shdr.size / 16;  // Elf32_Sym = 16 bytes
            for (uint32_t j = 0; j < symCount; j++) {
                uint32_t st_name = isLittleEndian ?
                    *(uint32_t*)&symData[j * 16] :
                    bswap32(*(uint32_t*)&symData[j * 16]);
                uint32_t st_value = isLittleEndian ?
                    *(uint32_t*)&symData[j * 16 + 4] :
                    bswap32(*(uint32_t*)&symData[j * 16 + 4]);
                uint32_t st_size = isLittleEndian ?
                    *(uint32_t*)&symData[j * 16 + 8] :
                    bswap32(*(uint32_t*)&symData[j * 16 + 8]);
                uint8_t st_info = symData[j * 16 + 12];

                uint8_t bind = st_info >> 4;
                uint8_t type = st_info & 0x0F;

                // 只处理函数和对象
                if (type == STT_FUNC || type == STT_OBJECT) {
                    // 查找符号名
                    if (st_name < strTabSize) {
                        std::string name = &strTab[st_name];

                        // 过滤特殊符号
                        if (name.empty() || name[0] == '$' || name == ".hidden") {
                            continue;
                        }

                        // 添加函数
                        ORBMDK_SymbolFuncInt func;
                        func.name = name;
                        func.startAddr = st_value;
                        func.endAddr = st_value + st_size;
                        func.fileIndex = 0;
                        ss->functions.push_back(func);
                    }
                }
            }
        }
    }

    fclose(f);

    return (struct ORBMDK_SymbolSet*)ss;
}

// ============================================================================
// 公共 API 实现
// ============================================================================

void ORBMDK_SymbolSet_Destroy(struct ORBMDK_SymbolSet* ss_)
{
    if (!ss_) return;

    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    if (ss->objdumpPipe) {
        _pclose(ss->objdumpPipe);
    }

    delete ss;
}

bool ORBMDK_SymbolSet_Valid(struct ORBMDK_SymbolSet* ss_)
{
    if (!ss_) return false;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;
    return !ss->elfFile.empty();
}

bool ORBMDK_Symbol_Lookup(struct ORBMDK_SymbolSet* ss_,
    uint32_t addr,
    struct ORBMDK_SymbolLookupResult* result)
{
    if (!ss_ || !result) return false;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    memset(result, 0, sizeof(*result));
    result->address = addr;

    if (ss->useObjdump) {
        // Objdump 模式
        if (!ss->objdumpPipe) return false;

        // 从缓存位置继续搜索
        // 简化实现: 从头开始扫描
        fseek(ss->objdumpPipe, 0, SEEK_SET);
        ss->cachedSearchIndex = 0;

        char line[MAX_LINE_LEN];
        std::string currentFunc;
        uint32_t currentFuncAddr = 0;
        char currentFile[MAX_LINE_LEN] = "";
        uint32_t currentLine = 0;
        bool inFunction = false;

        while (readObjdumpLine(ss->objdumpPipe, line, sizeof(line))) {
            LineType lt = getLineType(line);

            switch (lt) {
            case LT_PROC_LABEL: {
                currentFunc = extractFunctionName(line);
                // 解析地址
                const char* start = strchr(line, '0');
                if (start) {
                    currentFuncAddr = strtoul(start, nullptr, 0);
                }
                inFunction = true;

                // 添加函数
                ORBMDK_SymbolFuncInt func;
                func.name = currentFunc;
                func.startAddr = currentFuncAddr;
                func.fileIndex = 0;
                ss->functions.push_back(func);
                break;
            }

            case LT_FILEANDLINE: {
                // 格式: "c:/path/file.c:10"
                const char* colon = strrchr(line, ':');
                if (colon) {
                    strncpy(currentFile, line, colon - line);
                    currentFile[colon - line] = '\0';
                    currentLine = atoi(colon + 1);

                    // 添加文件
                    bool found = false;
                    for (auto& f : ss->files) {
                        if (f.name == currentFile) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        ORBMDK_SymbolFileInt file;
                        file.name = currentFile;
                        file.index = static_cast<uint32_t>(ss->files.size());
                        ss->files.push_back(file);
                    }
                }
                break;
            }

            case LT_SOURCE: {
                // 解析源码标记
                char fname[MAX_LINE_LEN];
                uint32_t lineNo = 0;
                parseSourceComment(line, fname, sizeof(fname), &lineNo);

                if (lineNo > 0) {
                    // 添加源码行
                    ORBMDK_SymbolLineInt sl;
                    sl.lineNo = lineNo;
                    sl.fileIndex = 0;
                    sl.functionIndex = ss->functions.size() > 0 ? static_cast<uint32_t>(ss->functions.size() - 1) : 0;
                    ss->lines.push_back(sl);
                }
                break;
            }

            case LT_ASSEMBLY: {
                if (!inFunction) break;

                // 解析汇编地址
                const char* addrStart = strstr(line, "0x");
                if (addrStart) {
                    uint32_t asmAddr = strtoul(addrStart, nullptr, 0);

                    // 检查是否匹配
                    if (asmAddr == addr) {
                        result->functionName = currentFunc.c_str();
                        result->lineNo = currentLine;
                        result->fileName = currentFile;
                        result->address = addr;
                        return true;
                    }
                }
                break;
            }

            default:
                break;
            }
        }

        return false;
    } else {
        // DWARF/符号表模式
        // 二分查找函数
        int left = 0;
        int right = (int)ss->functions.size() - 1;

        while (left <= right) {
            int mid = (left + right) / 2;
            const auto& func = ss->functions[mid];

            if (addr >= func.startAddr && addr < func.endAddr) {
                result->functionName = func.name.c_str();
                result->functionIndex = mid;

                // 查找源文件行
                for (const auto& line : ss->lines) {
                    if (addr >= line.startAddr && addr < line.endAddr) {
                        result->lineNo = line.lineNo;
                        if (line.fileIndex < ss->files.size()) {
                            result->fileName = ss->files[line.fileIndex].name.c_str();
                        }
                        break;
                    }
                }

                return true;
            } else if (addr < func.startAddr) {
                right = mid - 1;
            } else {
                left = mid + 1;
            }
        }

        return false;
    }
}

const char* ORBMDK_Symbol_Filename(struct ORBMDK_SymbolSet* ss_, uint32_t index)
{
    if (!ss_) return nullptr;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    if (index >= ss->files.size()) return nullptr;
    return ss->files[index].name.c_str();
}

const char* ORBMDK_Symbol_Function(struct ORBMDK_SymbolSet* ss_, uint32_t index)
{
    if (!ss_) return nullptr;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    if (index >= ss->functions.size()) return nullptr;
    return ss->functions[index].name.c_str();
}

const struct ORBMDK_SymbolFunc* ORBMDK_Symbol_FunctionAt(struct ORBMDK_SymbolSet* ss_, uint32_t addr)
{
    if (!ss_) return nullptr;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    for (const auto& func : ss->functions) {
        if (addr >= func.startAddr && addr < func.endAddr) {
            strncpy(g_funcNameBuf, func.name.c_str(), sizeof(g_funcNameBuf) - 1);
            g_funcNameBuf[sizeof(g_funcNameBuf) - 1] = 0;
            g_funcResult.name = g_funcNameBuf;
            g_funcResult.startAddr = func.startAddr;
            g_funcResult.endAddr = func.endAddr;
            g_funcResult.fileIndex = func.fileIndex;
            return &g_funcResult;
        }
    }

    return nullptr;
}

const struct ORBMDK_SymbolLine* ORBMDK_Symbol_LineAt(struct ORBMDK_SymbolSet* ss_, uint32_t addr)
{
    if (!ss_) return nullptr;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    for (const auto& line : ss->lines) {
        if (addr >= line.startAddr && addr < line.endAddr) {
            g_lineResult.startAddr = line.startAddr;
            g_lineResult.endAddr = line.endAddr;
            g_lineResult.lineNo = line.lineNo;
            g_lineResult.functionIndex = line.functionIndex;
            g_lineResult.fileIndex = line.fileIndex;
            return &g_lineResult;
        }
    }

    return nullptr;
}

size_t ORBMDK_Symbol_Disassemble(struct ORBMDK_SymbolSet* ss_, uint32_t addr,
    char* buffer, size_t bufferSize)
{
    if (!ss_ || !buffer || bufferSize == 0) return 0;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    // 使用 objdump 反汇编指定地址
    char commandLine[MAX_LINE_LEN];
    snprintf(commandLine, sizeof(commandLine),
        OBJDUMP_PATH " -d --start-address=0x%08X --stop-address=0x%08X %s",
        addr, addr + 16, ss->elfFile.c_str());

    FILE* pipe = _popen(commandLine, "r");
    if (!pipe) return 0;

    size_t total = 0;
    char line[MAX_LINE_LEN];

    while (readObjdumpLine(pipe, line, sizeof(line)) && total < bufferSize - 1) {
        LineType lt = getLineType(line);
        if (lt == LT_ASSEMBLY) {
            size_t len = strlen(line);
            if (total + len + 1 < bufferSize) {
                strcpy(buffer + total, line);
                strcat(buffer + total, "\n");
                total += len + 1;
            }
        }
    }

    _pclose(pipe);
    return total;
}

uint32_t ORBMDK_SymbolSet_GetFileCount(struct ORBMDK_SymbolSet* ss_)
{
    if (!ss_) return 0;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;
    return (uint32_t)ss->files.size();
}

uint32_t ORBMDK_SymbolSet_GetFunctionCount(struct ORBMDK_SymbolSet* ss_)
{
    if (!ss_) return 0;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;
    return (uint32_t)ss->functions.size();
}

uint32_t ORBMDK_SymbolSet_GetLineCount(struct ORBMDK_SymbolSet* ss_)
{
    if (!ss_) return 0;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;
    return (uint32_t)ss->lines.size();
}

void ORBMDK_SymbolSet_IterateFiles(struct ORBMDK_SymbolSet* ss_,
    void (*callback)(const struct ORBMDK_SymbolFile* file, void* userData),
    void* userData)
{
    if (!ss_ || !callback) return;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    struct ORBMDK_SymbolFile file;
    for (const auto& f : ss->files) {
        file.name = f.name.c_str();
        file.index = f.index;
        callback(&file, userData);
    }
}

void ORBMDK_SymbolSet_IterateFunctions(struct ORBMDK_SymbolSet* ss_,
    void (*callback)(const struct ORBMDK_SymbolFunc* func, void* userData),
    void* userData)
{
    if (!ss_ || !callback) return;
    struct ORBMDK_SymbolSetInt* ss = (struct ORBMDK_SymbolSetInt*)ss_;

    struct ORBMDK_SymbolFunc func;
    for (const auto& f : ss->functions) {
        func.name = f.name.c_str();
        func.startAddr = f.startAddr;
        func.endAddr = f.endAddr;
        func.fileIndex = f.fileIndex;
        callback(&func, userData);
    }
}

// ============================================================================
// 工具路径检测
// ============================================================================

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")

// 检查文件是否存在
static bool fileExists(const char* path) {
    return PathFileExistsA(path) != 0;
}

// 检查路径是否是有效的 objdump
static bool isValidObjdump(const char* path) {
    if (!fileExists(path)) return false;
    // 尝试运行 --version 检查
    char cmd[MAX_LINE_LEN];
    snprintf(cmd, sizeof(cmd), "\"%s\" --version 2>nul", path);
    FILE* f = _popen(cmd, "r");
    if (!f) return false;
    char buf[128] = {0};
    fgets(buf, sizeof(buf), f);
    _pclose(f);
    // 检查是否包含 arm 或 objdump
    return strstr(buf, "objdump") != nullptr || strstr(buf, "GNU") != nullptr;
}

// 从环境变量获取路径
static bool getEnvPath(const char* envName, char* buffer, size_t size) {
    const char* env = getenv(envName);
    if (!env || !env[0]) return false;

    char path[MAX_LINE_LEN];
    snprintf(path, sizeof(path), "%s\\bin\\arm-none-eabi-objdump.exe", env);
    if (isValidObjdump(path)) {
        strncpy(buffer, path, size);
        buffer[size - 1] = 0;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 本 DLL 的自身位置 —— 部署形态是 <KeilRoot>\ARM\BIN\CMSIS_DAP.dll，
// 于是"上两级"就是 Keil 安装根，可直接拿到 <KeilRoot>\TOOLS.INI，
// 不必再猜盘符（P1/P2 的硬路径问题的根治办法）。
//
// 用本文件函数的地址反查模块：既不依赖 DllMain 传参，也避开了
// GetModuleFileNameA(NULL, ...) 返回**宿主 UV4.exe** 的坑。
// ---------------------------------------------------------------------------
static bool getSelfModulePath(char* out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = 0;

    HMODULE hSelf = nullptr;
    const LPCSTR selfAddr = reinterpret_cast<LPCSTR>(&getSelfModulePath);
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            selfAddr, &hSelf) || !hSelf) {
        return false;
    }
    const DWORD n = GetModuleFileNameA(hSelf, out, (DWORD)cap);
    return n > 0 && n < (DWORD)cap;
}

// 剥掉路径里的文件名（原地）
static void stripFileName(char* path)
{
    char* slash = strrchr(path, '\\');
    if (slash) *slash = 0;
}

// 本 DLL 所在目录（<KeilRoot>\ARM\BIN）
static bool getSelfModuleDir(char* out, size_t cap)
{
    if (!getSelfModulePath(out, cap)) return false;
    stripFileName(out);
    return out[0] != 0;
}

// 在一条 TOOLS.INI 里找 GNU 工具链的 objdump（按 PATH= 行拼 <path>\bin\arm-none-eabi-objdump.exe）
static bool extractGnuFromToolsIni(const char* iniPath, char* out, size_t cap)
{
    FILE* f = fopen(iniPath, "r");
    if (!f) return false;

    bool found = false;
    char line[512];
    while (!found && fgets(line, sizeof(line), f)) {
        if (strstr(line, "Arm GNU Toolchain") ||
            strstr(line, "GNU Arm Embedded") ||
            strstr(line, "ARMCC") == nullptr) {   // 跳过 ARMCC
            char* eq = strchr(line, '=');
            if (!eq) continue;
            char* path = eq + 1;
            while (*path == '"' || *path == ' ') path++;
            char* end = path + strlen(path) - 1;
            while (end > path && (*end == '"' || *end == '\n' || *end == '\r')) *end-- = 0;
            if (*path) {
                char toolPath[MAX_LINE_LEN];
                snprintf(toolPath, sizeof(toolPath), "%sbin\\arm-none-eabi-objdump.exe", path);
                for (char* p = toolPath; *p; p++) {
                    if (*p == '/') *p = '\\';
                }
                if (isValidObjdump(toolPath)) {
                    strncpy(out, toolPath, cap);
                    out[cap - 1] = 0;
                    found = true;
                }
            }
        }
    }
    fclose(f);
    return found;
}

// 在 "<root>\*" 的每个子目录里找 bin\arm-none-eabi-objdump.exe
// （不写死版本号：工具链升级/换目录后仍能找到）
static bool scanGnuToolchainRoot(const char* rootWildcard, char* out, size_t cap)
{
    char root[MAX_LINE_LEN];
    strncpy(root, rootWildcard, sizeof(root) - 1);
    root[sizeof(root) - 1] = 0;
    char* star = strrchr(root, '*');
    if (star) *star = 0;                       // 只留根目录（含尾部分隔符）

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(rootWildcard, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;  // . / ..

        char cand[MAX_LINE_LEN];
        snprintf(cand, sizeof(cand), "%s%s\\bin\\arm-none-eabi-objdump.exe",
                 root, fd.cFileName);
        if (isValidObjdump(cand)) {
            strncpy(out, cand, cap);
            out[cap - 1] = 0;
            found = true;
            break;
        }
    } while (FindNextFileA(h, &fd));

    FindClose(h);
    return found;
}

bool ORBMDK_FindObjdumpPath(char* buffer, size_t bufferSize) {
    if (!buffer || bufferSize < MAX_LINE_LEN) return false;
    buffer[0] = 0;

    // 1. 检查 OBJDUMP 环境变量
    if (getEnvPath("OBJDUMP", buffer, bufferSize)) return true;

    // 2. 检查 ARM_TOOLCHAIN_PATH
    if (getEnvPath("ARM_TOOLCHAIN_PATH", buffer, bufferSize)) return true;

    // 3. 检查 ARMGCC_DIR
    if (getEnvPath("ARMGCC_DIR", buffer, bufferSize)) return true;

    // 4. 解析 Keil TOOLS.INI 获取工具链路径
    // 4a. **优先：由本 DLL 位置反推**（零猜测）—— DLL 部署在 <KeilRoot>\ARM\BIN，
    //     故 <selfDir>\..\..\TOOLS.INI 就是 Keil 根的 TOOLS.INI。
    {
        char selfDir[MAX_PATH] = {};
        if (getSelfModuleDir(selfDir, sizeof(selfDir))) {
            char iniRaw[MAX_LINE_LEN];
            snprintf(iniRaw, sizeof(iniRaw), "%s\\..\\..\\TOOLS.INI", selfDir);
            char ini[MAX_PATH] = {};
            if (PathCanonicalizeA(ini, iniRaw) && fileExists(ini) &&
                extractGnuFromToolsIni(ini, buffer, bufferSize)) {
                return true;
            }
        }
    }
    // 4b. 回退：常见安装位置（猜路径；仅在 4a 拿不到时用）
    const char* mdkPaths[] = {
        "C:\\Program Files\\Keil_v5\\TOOLS.INI",
        "C:\\Program Files (x86)\\Keil_v5\\TOOLS.INI",
        "D:\\Keil_v5\\TOOLS.INI",
        "C:\\Keil_v5\\TOOLS.INI",
        "D:\\MDK5\\TOOLS.INI"
    };
    for (size_t i = 0; i < sizeof(mdkPaths) / sizeof(mdkPaths[0]); i++) {
        if (fileExists(mdkPaths[i]) &&
            extractGnuFromToolsIni(mdkPaths[i], buffer, bufferSize)) {
            return true;
        }
    }

    // 5. GNU 工具链：按"根目录 + 版本通配"扫描（**不写死版本号/盘符**，升级后照旧可用）
    {
        char pattern[MAX_LINE_LEN];

        // 5a. Keil 根下自带的位置（<KeilRoot>\ARM\ARM_GCC\* / ...\ARM\GNU Toolchain\*）
        char selfDir[MAX_PATH] = {};
        if (getSelfModuleDir(selfDir, sizeof(selfDir))) {
            snprintf(pattern, sizeof(pattern), "%s\\..\\..\\ARM\\ARM_GCC\\*", selfDir);
            if (scanGnuToolchainRoot(pattern, buffer, bufferSize)) return true;
            snprintf(pattern, sizeof(pattern), "%s\\..\\..\\ARM\\GNU Toolchain\\*", selfDir);
            if (scanGnuToolchainRoot(pattern, buffer, bufferSize)) return true;
        }

        // 5b. 常见安装根（用环境变量拼，不写死 "C:\Program Files"）
        const char* roots[] = {
            getenv("ProgramFiles"), getenv("ProgramW6432"), getenv("ProgramFiles(x86)")
        };
        const char* subdirs[] = {
            "\\Arm\\GNU Toolchain\\*",
            "\\GNU Arm Embedded Toolchain\\*",
        };
        for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); ++r) {
            if (!roots[r] || !roots[r][0]) continue;
            for (size_t s = 0; s < sizeof(subdirs) / sizeof(subdirs[0]); ++s) {
                snprintf(pattern, sizeof(pattern), "%s%s", roots[r], subdirs[s]);
                if (scanGnuToolchainRoot(pattern, buffer, bufferSize)) return true;
            }
        }
    }

    // 6. 从 PATH 环境变量搜索
    const char* pathEnv = getenv("PATH");
    if (pathEnv) {
        char* pathCopy = _strdup(pathEnv);
        char* ctx = nullptr;
        char* dir = strtok_s(pathCopy, ";", &ctx);
        while (dir) {
            char objdumpPath[MAX_LINE_LEN];
            snprintf(objdumpPath, sizeof(objdumpPath), "%s\\arm-none-eabi-objdump.exe", dir);
            if (isValidObjdump(objdumpPath)) {
                strncpy(buffer, objdumpPath, bufferSize);
                buffer[bufferSize - 1] = 0;
                free(pathCopy);
                return true;
            }
            dir = strtok_s(nullptr, ";", &ctx);
        }
        free(pathCopy);
    }

    // 7. 使用默认名称 (依赖 PATH)
    strncpy(buffer, OBJDUMP_PATH, bufferSize);
    buffer[bufferSize - 1] = 0;
    return true;  // 返回默认名称，是否有效由调用者决定
}
#endif // _WIN32
