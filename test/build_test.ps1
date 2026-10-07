# ORBMDK RDDI Test Build Script
# PowerShell version - 参考项目根目录 build.ps1 风格

param(
    [string]$Source = "swdprobe.cpp",
    [switch]$All      # 重建 test\ 下**全部**工具（改完 DLL 后常用）
)

# test\ 目录的完整工具清单（2026-09-30 整理后的命名）
$AllSources = @(
    "ORBMDK_RDDI_FullTest.cpp",     # 全面功能测试（40 项，含完整导出扫描）
    "ORBMDK_BlockTransferTest.cpp", # 块传输提速/越界验证
    "ORBMDK_RAM_SpeedTest.cpp",     # RAM 读写吞吐（绕开 Keil/AGDI，测设备侧极限）
    "swdprobe.cpp",                 # SWD 通路功能回归（V2 或 V1，见用法）
    "hidprobe.cpp",                 # CMSIS-DAP v1 (HID) 极简回归
    "jtagprobe.cpp",                # JTAG 端到端（走本层 DLL）
    "jtagrawprobe.cpp",             # JTAG 裸帧/引脚级诊断（绕开本层）
    "jtagblockprobe.cpp",           # JTAG 下 ID_DAP_TRANSFER_BLOCK(0x06) 专项复验（绕开本层）
    "v2rawprobe.cpp"                # V2 裸帧/包长/分片诊断（绕开本层）
)

# -All：逐个重建（改完 DLL 后一次把工具全刷一遍）
if ($All) {
    $failed = 0
    foreach ($s in $AllSources) {
        Write-Host ""
        Write-Host "########## $s ##########" -ForegroundColor Cyan
        & $PSCommandPath -Source $s
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) { $failed++ }
    }
    Write-Host ""
    if ($failed -eq 0) { Write-Host "===== ALL BUILD SUCCESSFUL =====" -ForegroundColor Green; exit 0 }
    Write-Host "===== $failed BUILD(S) FAILED =====" -ForegroundColor Red
    exit 1
}

$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "ORBMDK RDDI Test Build (MSVC x86)"
Write-Host "========================================"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# 创建输出目录
$BinDir = Join-Path $ScriptDir "..\bin"
if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

# ----------------------------------------------------------------------------
# 工具链定位（x86）：vswhere -> 常见安装目录；VC 工具集 / Windows SDK 取最高版本
# 与根目录 build.ps1 保持一致（本项目脚本按同构方式维护）
# ----------------------------------------------------------------------------
function Find-VSDir {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $p = & $vswhere -latest -products * `
            -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
            -property installationPath 2>$null
        if ($p) {
            $cand = ($p | Select-Object -First 1).Trim()
            if (Test-Path -LiteralPath "$cand\VC\Tools\MSVC") { return $cand }
        }
    }
    foreach ($c in @(
        "D:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "D:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "D:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community",
        "C:\Program Files (x86)\Microsoft Visual Studio\2017\Community")) {
        if (Test-Path -LiteralPath "$c\VC\Tools\MSVC") { return $c }
    }
    return $null
}

function Get-NewestVersionDir {
    param([string]$Parent, [string]$Pattern)
    if (-not (Test-Path -LiteralPath $Parent)) { return $null }
    Get-ChildItem -LiteralPath $Parent -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match $Pattern } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}

$MSVCDir = Find-VSDir
if (-not $MSVCDir) {
    Write-Host "[ERROR] Visual Studio C++ toolchain not found." -ForegroundColor Red
    Write-Host "        Install VS 2017+ with 'Desktop development with C++'." -ForegroundColor Red
    exit 1
}

$VCTools = Get-NewestVersionDir -Parent "$MSVCDir\VC\Tools\MSVC" -Pattern '^\d+(\.\d+)*$'
if (-not $VCTools) {
    Write-Host "[ERROR] VC tools not found under $MSVCDir\VC\Tools\MSVC" -ForegroundColor Red
    exit 1
}

$CompilerBin = "$VCTools\bin\Hostx86\x86"
$Compiler = Join-Path $CompilerBin "cl.exe"
$Linker = Join-Path $CompilerBin "link.exe"

# Windows SDK 根目录：注册表 KitsRoot10 -> 常见目录
$sdkRoot = $null
foreach ($key in @("HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots",
                   "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots")) {
    if (Test-Path $key) {
        $root = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).KitsRoot10
        if ($root -and (Test-Path -LiteralPath $root)) { $sdkRoot = $root; break }
    }
}
if (-not $sdkRoot) {
    foreach ($c in @("D:\Windows Kits\10", "C:\Program Files (x86)\Windows Kits\10")) {
        if (Test-Path -LiteralPath $c) { $sdkRoot = $c; break }
    }
}
$sdkVer = $null
if ($sdkRoot) {
    $sdkVer = Get-ChildItem -LiteralPath "$sdkRoot\Include" -Directory -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^10\.\d+' -and (Test-Path -LiteralPath "$sdkRoot\Lib\$($_.Name)\ucrt\x86") } |
        Sort-Object { [version]$_.Name } -Descending |
        Select-Object -First 1 -ExpandProperty Name
}
if (-not $sdkVer) {
    Write-Host "[ERROR] Windows SDK (10.x) not found." -ForegroundColor Red
    exit 1
}
$WindowsSDKInclude = Join-Path $sdkRoot "Include\$sdkVer"
$WindowsSDKLib = Join-Path $sdkRoot "Lib\$sdkVer"

$env:Path = "$CompilerBin;$MSVCDir\Common7\IDE;$env:Path"

Write-Host "MSVC Directory: $MSVCDir"
Write-Host "VC Tools: $VCTools"
Write-Host "Windows SDK Include: $WindowsSDKInclude"
Write-Host "Windows SDK Lib: $WindowsSDKLib"
Write-Host ""

$SourceFile = Join-Path $ScriptDir $Source
$SourceBaseName = [System.IO.Path]::GetFileNameWithoutExtension($Source)
$ExeFile = Join-Path $BinDir "$SourceBaseName.exe"
$ObjFile = Join-Path $ScriptDir "$SourceBaseName.obj"

# Compiler flags（/O2 的口径见 build.ps1；测试工具与 DLL 保持同一优化等级）
$CompilerFlags = "/c /nologo /O2 /MD /W3 /EHsc /std:c++17 /utf-8"
$CompilerFlags += " /D_WINDOWS"
$CompilerFlags += " /I`"$ScriptDir\..\include`""
$CompilerFlags += " /I`"$VCTools\include`""
$CompilerFlags += " /I`"$WindowsSDKInclude\ucrt`""
$CompilerFlags += " /I`"$WindowsSDKInclude\shared`""
$CompilerFlags += " /I`"$WindowsSDKInclude\um`""
$CompilerFlags += " /I`"$WindowsSDKInclude\winrt`""

# Linker flags
$LinkerFlags = "/NOLOGO"
$LinkerFlags += " /OUT:`"$ExeFile`""
$LinkerFlags += " /LIBPATH:`"$VCTools\lib\x86`""
$LinkerFlags += " /LIBPATH:`"$WindowsSDKLib\ucrt\x86`""
$LinkerFlags += " /LIBPATH:`"$WindowsSDKLib\um\x86`""
$LinkerFlags += " user32.lib"

Write-Host "Source: $SourceFile"
Write-Host "Output: $ExeFile"
Write-Host ""

# 编译
Write-Host "========================================"
Write-Host "Compiling..."
Write-Host "========================================"
Write-Host ""

if (-not (Test-Path $SourceFile)) {
    Write-Host "ERROR: Source file not found: $SourceFile" -ForegroundColor Red
    exit 1
}

Write-Host "Compiling: $(Split-Path $SourceFile -Leaf)..."
$cmd = "& `"$Compiler`" $CompilerFlags /Fo`"$ObjFile`" `"$SourceFile`""
$output = Invoke-Expression $cmd 2>&1
$output | ForEach-Object {
    if ($_ -match "error") {
        Write-Host $_ -ForegroundColor Red
    } elseif ($_ -match "warning") {
        Write-Host $_ -ForegroundColor Yellow
    }
}

if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) {
    Write-Host "COMPILE FAILED!" -ForegroundColor Red
    exit 1
}
Write-Host "Compile OK" -ForegroundColor Green

# 链接
Write-Host ""
Write-Host "========================================"
Write-Host "Linking..."
Write-Host "========================================"
Write-Host ""

$cmd = "& `"$Linker`" $LinkerFlags `"$ObjFile`""
$output = Invoke-Expression $cmd 2>&1
$output | ForEach-Object {
    if ($_ -match "error") {
        Write-Host $_ -ForegroundColor Red
    } elseif ($_ -match "warning") {
        Write-Host $_ -ForegroundColor Yellow
    }
}

# 清理 obj
if (Test-Path $ObjFile) { Remove-Item $ObjFile -Force }

if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) {
    Write-Host ""
    Write-Host "LINK FAILED!" -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "========================================"
Write-Host "Build SUCCESSFUL!" -ForegroundColor Green
Write-Host "Output: $ExeFile" -ForegroundColor Green
Write-Host "========================================"
