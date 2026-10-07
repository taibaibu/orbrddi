# ORBMDK Build Script for MSVC 2017
# PowerShell version - Fixed paths

$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "ORBMDK Build Script (MSVC 2017 x86)"
Write-Host "========================================"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# Create directories
$ObjDir = Join-Path $ScriptDir "obj"
$BinDir = Join-Path $ScriptDir "bin"
if (-not (Test-Path $ObjDir)) { New-Item -ItemType Directory -Path $ObjDir | Out-Null }
if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

# MSVC 2017 Paths
$MSVCDir = "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community"
$VCTools = "$MSVCDir\VC\Tools\MSVC\14.16.27023"
$CompilerBin = "$VCTools\bin\Hostx86\x86"
$Compiler = Join-Path $CompilerBin "cl.exe"
$Linker = Join-Path $CompilerBin "link.exe"

# Add to PATH
$env:Path = "$CompilerBin;$MSVCDir\Common7\IDE;$env:Path"

# Windows SDK
$WindowsSDKInclude = "D:\Windows Kits\10\Include\10.0.17763.0"
$WindowsSDKLib = "D:\Windows Kits\10\Lib\10.0.17763.0"

Write-Host "MSVC Directory: $MSVCDir"
Write-Host "VC Tools: $VCTools"
Write-Host "Windows SDK Include: $WindowsSDKInclude"
Write-Host "Windows SDK Lib: $WindowsSDKLib"
Write-Host ""

# Compiler flags - use /EHsc to enable exception handling
$CompilerFlags = "/c /nologo /MD /W3 /EHsc /std:c++17 /utf-8"
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
