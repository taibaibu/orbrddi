# ORBMDK RDDI Test Build Script
# PowerShell version - 参考项目根目录 build.ps1 风格

param(
    [string]$Source = "ORBMDK_RDDI_Test.cpp"
)

$ErrorActionPreference = "Continue"

Write-Host "========================================"
Write-Host "ORBMDK RDDI Test Build (MSVC 2017 x86)"
Write-Host "========================================"
Write-Host ""

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $ScriptDir

# 创建输出目录
$BinDir = Join-Path $ScriptDir "..\bin"
if (-not (Test-Path $BinDir)) { New-Item -ItemType Directory -Path $BinDir | Out-Null }

# MSVC 2017 Paths
$MSVCDir = "D:\Program Files (x86)\Microsoft Visual Studio\2017\Community"
$VCTools = "$MSVCDir\VC\Tools\MSVC\14.16.27023"
$CompilerBin = "$VCTools\bin\Hostx86\x86"
$Compiler = Join-Path $CompilerBin "cl.exe"
$Linker = Join-Path $CompilerBin "link.exe"

$env:Path = "$CompilerBin;$MSVCDir\Common7\IDE;$env:Path"

# Windows SDK
$WindowsSDKInclude = "D:\Windows Kits\10\Include\10.0.17763.0"
$WindowsSDKLib = "D:\Windows Kits\10\Lib\10.0.17763.0"

Write-Host "MSVC Directory: $MSVCDir"
Write-Host "VC Tools: $VCTools"
Write-Host "Windows SDK Include: $WindowsSDKInclude"
Write-Host "Windows SDK Lib: $WindowsSDKLib"
Write-Host ""

$SourceFile = Join-Path $ScriptDir $Source
$SourceBaseName = [System.IO.Path]::GetFileNameWithoutExtension($Source)
$ExeFile = Join-Path $BinDir "$SourceBaseName.exe"
$ObjFile = Join-Path $ScriptDir "$SourceBaseName.obj"

# Compiler flags
$CompilerFlags = "/c /nologo /MD /W3 /EHsc /std:c++17 /utf-8"
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
