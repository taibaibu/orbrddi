# ORBMDK Build Script (MSVC x86)
# PowerShell version - toolchain paths are auto-detected
#
# 用法：
#   .\build.ps1                                        # 只编译
#   .\build.ps1 -Deploy                                # 编译成功后一键部署到 Keil
#   .\build.ps1 -DeployOnly                            # 只部署，不编译（用现有 bin\ORBMDK_RDDI.dll）
#   .\build.ps1 -Deploy -KeilArmBin "D:\Keil_v5\ARM\BIN" # 显式指定 Keil 的 ARM\BIN
#
# 部署（原 deploy.ps1，2026-10-02 合并进来）：把 bin\ORBMDK_RDDI.dll 覆盖 Keil 的
# CMSIS_DAP.dll，首次覆盖前把官方原件备份为 CMSIS_DAP.dll.bak。覆盖无需关掉 µVisio !!!。
#
# ⚠ 本文件必须保持 **UTF-8 with BOM + CRLF**：Windows PowerShell 5.1 对无 BOM 的脚本
#   按 ANSI(936) 解码，中文注释会乱码甚至语法报错（详见 COMPAT_ANALYSIS.md 构建脚本编码一节）。

param(
    [switch]$Deploy,            # 编译成功后自动部署到 Keil
    [switch]$DeployOnly,        # 只部署、不编译（隐含 -Deploy）
    [string]$KeilArmBin = ""    # Keil 的 ARM\BIN 目录；留空 = 自动探测
)

$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "ORBMDK Build Script (MSVC x86)"
Write-Host "========================================"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# -DeployOnly 只走部署分支，不编译（隐含 -Deploy）
if ($DeployOnly) { $Deploy = $true }

# Create directories
$ObjDir = Join-Path $ScriptDir "obj"
$BinDir = Join-Path $ScriptDir "bin"
if (-not (Test-Path $ObjDir)) { New-Item -ItemType Directory -Path $ObjDir | Out-Null }
if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

# ----------------------------------------------------------------------------
# 工具链定位（x86）：vswhere -> 常见安装目录；VC 工具集 / Windows SDK 取最高版本
# 不写死盘符、VS 版本、SDK 版本
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
# 取"同时具备 Include\<ver> 与 Lib\<ver>\ucrt\x86"的最高版本
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

# Add to PATH
$env:Path = "$CompilerBin;$MSVCDir\Common7\IDE;$env:Path"

Write-Host "MSVC Directory: $MSVCDir"
Write-Host "VC Tools: $VCTools"
Write-Host "Windows SDK Include: $WindowsSDKInclude"
Write-Host "Windows SDK Lib: $WindowsSDKLib"
Write-Host ""

# Compiler flags - use /EHsc to enable exception handling
#
# /MT (静态链接 CRT)，不要改回 /MD —— 原因见 COMPAT_ANALYSIS.md §12：
# Keil 的 ARMCLANG\bin 下自带一个旧的 MSVCP140.dll(14.29)，UV4 加载的是它。
# MSVC 运行时只保证"向前兼容"（旧工具集产物跑在新运行时上），不保证反向兼容；
# 用 VS2022(14.4x) 编译却跑在 14.29 上会直接崩在 MSVCP140.dll（0xc0000005）。
# /MT 让本 DLL 不再依赖 MSVCP140/VCRUNTIME140/UCRT，宿主进程里是哪个版本都无所谓。
# RDDI 是纯 C ABI（缓冲区均由调用方提供，不跨模块传 STL/堆指针），静态 CRT 是安全的。
#
# /O2（最大化速度）：脚本原先没写任何 /O*，MSVC 默认是 /Od（禁用优化，cl /? 里标注"默认"）。
# 实测瓶颈在 USB 往返（150~600 µs/次），CPU 侧只是 ns 级 —— 开 /O2 属"顺手拉满"，**不是提速手段**。
# 排障若想单步/断点更直观，去掉 /O2 即回到 /Od（只改这一处，其他开关都不用动）。
$CompilerFlags = "/c /nologo /O2 /MT /W3 /EHsc /std:c++17 /utf-8"
$CompilerFlags += " /D_WINDOWS /D_USRDLL /DORBMDK_EXPORTS /DWIN32 /D_WINDLL"
$CompilerFlags += " /DNOMINMAX /DWIN32_LEAN_AND_MEAN"
$CompilerFlags += " /D_CRT_SECURE_NO_WARNINGS"  # 禁用 deprecated 警告
$CompilerFlags += " /I`"$ScriptDir\include`""
$CompilerFlags += " /I`"$VCTools\include`""
$CompilerFlags += " /I`"$WindowsSDKInclude\ucrt`""
$CompilerFlags += " /I`"$WindowsSDKInclude\shared`""
$CompilerFlags += " /I`"$WindowsSDKInclude\um`""
$CompilerFlags += " /I`"$WindowsSDKInclude\winrt`""

# Linker flags - 输出 ORBMDK_RDDI.dll
$LinkerFlags = "/DLL /NOLOGO /MANIFEST:NO"
$LinkerFlags += " /OUT:`"$BinDir\ORBMDK_RDDI.dll`""
$LinkerFlags += " /LIBPATH:`"$VCTools\lib\x86`""
$LinkerFlags += " /LIBPATH:`"$WindowsSDKLib\ucrt\x86`""
$LinkerFlags += " /LIBPATH:`"$WindowsSDKLib\um\x86`""
$LinkerFlags += " kernel32.lib user32.lib advapi32.lib setupapi.lib winusb.lib"

Write-Host "Compiler flags: $CompilerFlags"
Write-Host ""

if ($DeployOnly) {
    # 只部署：不碰编译产物，直接用已有的 bin\ORBMDK_RDDI.dll
    $ErrorCount = 0
    Write-Host "========================================"
    Write-Host "跳过编译（-DeployOnly）"
    Write-Host "========================================"
    Write-Host ""
} else {
Write-Host "========================================"
Write-Host "Compiling ORBMDK..."
Write-Host "========================================"
Write-Host ""

$SrcDir = Join-Path $ScriptDir "src"
$ErrorCount = 0

function Compile-Source {
    param($SourceFile, $OutputFile)
    Write-Host "Compiling: $(Split-Path $SourceFile -Leaf)..."
    $cmd = "& `"$Compiler`" $CompilerFlags /Fo`"$OutputFile`" `"$SourceFile`""
    $output = Invoke-Expression $cmd 2>&1
    $output | ForEach-Object {
        if ($_ -match "error") {
            Write-Host $_ -ForegroundColor Red
        } elseif ($_ -match "warning") {
            Write-Host $_ -ForegroundColor Yellow
        }
    }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) {
        Write-Host "ERROR in $SourceFile" -ForegroundColor Red
        return $false
    }
    return $true
}

# Compile sources
$Sources = @(
    @{Name="pch.cpp"; Obj="$ObjDir\pch.obj"},
    @{Name="ORBMDK_DLL.cpp"; Obj="$ObjDir\ORBMDK_DLL.obj"},
    @{Name="ORBMDK_Log.cpp"; Obj="$ObjDir\ORBMDK_Log.obj"},
    @{Name="ORBMDK_HID.cpp"; Obj="$ObjDir\ORBMDK_HID.obj"},
    @{Name="ORBMDK_RDDI.cpp"; Obj="$ObjDir\ORBMDK_RDDI.obj"},
    @{Name="ORBMDK_USB_Bulk.cpp"; Obj="$ObjDir\ORBMDK_USB_Bulk.obj"},
    @{Name="ORBMDK_COBS.cpp"; Obj="$ObjDir\ORBMDK_COBS.obj"},
    @{Name="ORBMDK_OFLOW.cpp"; Obj="$ObjDir\ORBMDK_OFLOW.obj"},
    @{Name="ORBMDK_ITM_Decoder.cpp"; Obj="$ObjDir\ORBMDK_ITM_Decoder.obj"},
    @{Name="ORBMDK_TPIU_Decoder.cpp"; Obj="$ObjDir\ORBMDK_TPIU_Decoder.obj"},
    @{Name="ORBMDK_ETM_Decoder.cpp"; Obj="$ObjDir\ORBMDK_ETM_Decoder.obj"},
    @{Name="ORBMDK_Trace.cpp"; Obj="$ObjDir\ORBMDK_Trace.obj"},
    @{Name="ORBMDK_Symbols.cpp"; Obj="$ObjDir\ORBMDK_Symbols.obj"},
    @{Name="ORBMDK_Coverage.cpp"; Obj="$ObjDir\ORBMDK_Coverage.obj"}
)

foreach ($src in $Sources) {
    $SourcePath = Join-Path $SrcDir $src.Name
    if (-not (Compile-Source $SourcePath $src.Obj)) {
        $ErrorCount++
    }
}

Write-Host ""
Write-Host "========================================"
Write-Host "Linking..."
Write-Host "========================================"
Write-Host ""

if ($ErrorCount -eq 0) {
    $ObjFiles = ($Sources | ForEach-Object { $_.Obj }) -join " "
    $cmd = "& `"$Linker`" $LinkerFlags $ObjFiles"
    Write-Host "Linking $BinDir\ORBMDK_RDDI.dll..."
    $output = Invoke-Expression $cmd 2>&1
    $output | ForEach-Object {
        if ($_ -match "error") {
            Write-Host $_ -ForegroundColor Red
        } elseif ($_ -match "warning") {
            Write-Host $_ -ForegroundColor Yellow
        }
    }
    if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne $null) {
        $ErrorCount++
    }
}
}   # end of: if (-not $DeployOnly) —— 编译 + 链接

Write-Host ""
if ($ErrorCount -eq 0 -and (Test-Path "$BinDir\ORBMDK_RDDI.dll")) {
    Write-Host "========================================"
    Write-Host "Build SUCCESSFUL!" -ForegroundColor Green
    Write-Host "Output: $BinDir\ORBMDK_RDDI.dll"
    Write-Host "========================================"
} else {
    Write-Host "========================================"
    if ($DeployOnly) {
        Write-Host "没有可部署的产物：$BinDir\ORBMDK_RDDI.dll 不存在！" -ForegroundColor Red
        Write-Host "（-DeployOnly 不编译，请先运行 .\build.ps1）" -ForegroundColor Red
    } else {
        Write-Host "Build FAILED with $ErrorCount error(s)!" -ForegroundColor Red
    }
    Write-Host "========================================"
    exit 1
}

# ---------------------------------------------------------------------------
# 部署到 Keil（原 deploy.ps1，2026-10-02 合并到本脚本）
#
# 触发方式：-Deploy（编译成功后）或 -DeployOnly（不编译，直接部署现有产物）。
# 未触发时只在末尾打一句提示，不改变原有"只编译"行为。
# ---------------------------------------------------------------------------
function Find-KeilArmBin {
    param([string]$Override)
    if ($Override) {
        if (Test-Path -LiteralPath $Override) { return $Override }
        return $null
    }
    $candidates = @(
        "D:\Keil_v5\ARM\BIN",
        "C:\Keil_v5\ARM\BIN",
        "D:\MDK5\ARM\BIN",
        "C:\Program Files\Keil_v5\ARM\BIN",
        "C:\Program Files (x86)\Keil_v5\ARM\BIN"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $c "CMSIS_AGDI.dll")) { return $c }
    }
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}

# 部署：成功 $true / 失败 $false。
# 本脚本的 $ErrorActionPreference 是 "Continue"（编译阶段要容忍 cl.exe 的报错输出），
# 所以这里对每个关键 cmdlet 显式 -ErrorAction Stop 并 try/catch，不能依赖全局设置。
function Invoke-Deploy {
    param([string]$KeilBinOverride)

    $SourceDll = Join-Path $ScriptDir "bin\ORBMDK_RDDI.dll"
    Write-Host "========================================"
    Write-Host "ORBMDK Deploy - Replace CMSIS_DAP.dll"
    Write-Host "========================================"

    if (-not (Test-Path -LiteralPath $SourceDll)) {
        Write-Host "[ERROR] 源 DLL 不存在：$SourceDll" -ForegroundColor Red
        Write-Host "        请先编译（不要加 -DeployOnly）。" -ForegroundColor Red
        return $false
    }

    $KeilBin = Find-KeilArmBin -Override $KeilBinOverride
    if (-not $KeilBin) {
        Write-Host "[ERROR] 未找到 Keil 的 ARM\BIN 目录。" -ForegroundColor Red
        Write-Host "        请显式指定： .\build.ps1 -Deploy -KeilArmBin 'D:\Keil_v5\ARM\BIN'" -ForegroundColor Red
        return $false
    }

    $TargetDll = Join-Path $KeilBin "CMSIS_DAP.dll"
    $BackupDll = "$TargetDll.bak"
    Write-Host "Source: $SourceDll"
    Write-Host "Target: $TargetDll"

    # 权限提示（仅提示；非管理员也可能成功，取决于目标目录 ACL）
    $IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $IsAdmin) {
        Write-Host "[WARN] 当前非管理员，仍会尝试覆盖。" -ForegroundColor Yellow
        Write-Host "       失败请用管理员 PowerShell 重跑，并先关闭 µVision。" -ForegroundColor Yellow
    }

    # 官方原件只备份一次（备份已存在就不再覆盖，避免把我们的 DLL 备成 .bak）
    if (Test-Path -LiteralPath $TargetDll) {
        if (Test-Path -LiteralPath $BackupDll) {
            Write-Host "[SKIP] 备份已存在：$BackupDll"
        } else {
            try {
                Copy-Item -LiteralPath $TargetDll -Destination $BackupDll -Force -ErrorAction Stop
                Write-Host "[ OK ] 已备份 Keil 原件：$BackupDll" -ForegroundColor Green
            } catch {
                Write-Host "[ERROR] 备份失败：$($_.Exception.Message)" -ForegroundColor Red
                return $false
            }
        }
    } else {
        Write-Host "[WARN] 目标文件不存在，将新建。" -ForegroundColor Yellow
    }

    try {
        Copy-Item -LiteralPath $SourceDll -Destination $TargetDll -Force -ErrorAction Stop
        Write-Host "[ OK ] 已覆盖：$TargetDll" -ForegroundColor Green
    } catch {
        Write-Host "[ERROR] 覆盖失败：$($_.Exception.Message)" -ForegroundColor Red
        Write-Host "        请关闭 Keil µVision 后重试（DLL 被占用时无法覆盖）。" -ForegroundColor Red
        return $false
    }

    # 用 SHA256 核对是否真的落盘（同机复制，不一致说明拷贝没生效）
    try {
        $SrcHash = (Get-FileHash -LiteralPath $SourceDll -Algorithm SHA256 -ErrorAction Stop).Hash
        $DstHash = (Get-FileHash -LiteralPath $TargetDll -Algorithm SHA256 -ErrorAction Stop).Hash
    } catch {
        Write-Host "[ERROR] 计算哈希失败：$($_.Exception.Message)" -ForegroundColor Red
        return $false
    }
    if ($SrcHash -ne $DstHash) {
        Write-Host "[ERROR] 拷贝后哈希不一致！" -ForegroundColor Red
        Write-Host "  Source: $SrcHash"
        Write-Host "  Target: $DstHash"
        return $false
    }

    Write-Host ""
    Write-Host "========================================"
    Write-Host "Deploy SUCCESSFUL!" -ForegroundColor Green
    Write-Host "Target: $TargetDll"
    Write-Host "SHA256: $DstHash"
    Write-Host "还原： Copy-Item '$BackupDll' '$TargetDll' -Force"
    Write-Host "========================================"
    return $true
}

if ($Deploy) {
    Write-Host ""
    if (-not (Invoke-Deploy -KeilBinOverride $KeilArmBin)) {
        Write-Host ""
        Write-Host "Deploy FAILED!" -ForegroundColor Red
        exit 1
    }
} else {
    Write-Host ""
    Write-Host "提示：加 -Deploy 可在编译成功后一键部署到 Keil： .\build.ps1 -Deploy" -ForegroundColor DarkGray
}
