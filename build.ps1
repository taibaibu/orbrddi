# ORBMDK Build Script (MSVC x86)
# PowerShell version - toolchain paths are auto-detected

$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "ORBMDK Build Script (MSVC x86)"
Write-Host "========================================"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

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
$CompilerFlags = "/c /nologo /MT /W3 /EHsc /std:c++17 /utf-8"
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

Write-Host ""
if ($ErrorCount -eq 0 -and (Test-Path "$BinDir\ORBMDK_RDDI.dll")) {
    Write-Host "========================================"
    Write-Host "Build SUCCESSFUL!" -ForegroundColor Green
    Write-Host "Output: $BinDir\ORBMDK_RDDI.dll"
    Write-Host "========================================"
} else {
    Write-Host "========================================"
    Write-Host "Build FAILED with $ErrorCount error(s)!" -ForegroundColor Red
    Write-Host "========================================"
    exit 1
}
