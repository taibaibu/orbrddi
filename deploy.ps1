# ORBMDK Deploy Script
# Copy bin\ORBMDK_RDDI.dll over Keil MDK's CMSIS_DAP.dll
# Usage: run in PowerShell:  .\deploy.ps1
#        .\deploy.ps1 -KeilArmBin "D:\Keil_v5\ARM\BIN"    # 手动指定 Keil ARM\BIN
#        (run as Administrator if the copy is denied)

param(
    [string]$KeilArmBin = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$SourceDll = Join-Path $ScriptDir "bin\ORBMDK_RDDI.dll"

# 定位 Keil 的 ARM\BIN：显式参数 > 常见安装目录
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

$KeilArmBin = Find-KeilArmBin -Override $KeilArmBin
if (-not $KeilArmBin) {
    Write-Host "[ERROR] Keil ARM\BIN not found." -ForegroundColor Red
    Write-Host "        Pass it explicitly:  .\deploy.ps1 -KeilArmBin 'D:\Keil_v5\ARM\BIN'" -ForegroundColor Red
    exit 1
}

$TargetDll = Join-Path $KeilArmBin "CMSIS_DAP.dll"
$BackupDll = "$TargetDll.bak"

Write-Host "========================================"
Write-Host "ORBMDK Deploy - Replace CMSIS_DAP.dll"
Write-Host "========================================"
Write-Host "Source: $SourceDll"
Write-Host "Target: $TargetDll"
Write-Host ""

# Check source file
if (-not (Test-Path -LiteralPath $SourceDll)) {
    Write-Host "[ERROR] Source DLL not found: $SourceDll" -ForegroundColor Red
    Write-Host "        Please run build.ps1 first." -ForegroundColor Red
    exit 1
}

# Check target directory
$TargetDir = Split-Path -Parent $TargetDll
if (-not (Test-Path -LiteralPath $TargetDir)) {
    Write-Host "[ERROR] Target directory not found: $TargetDir" -ForegroundColor Red
    exit 1
}

# Admin check (warning only)
$IsAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $IsAdmin) {
    Write-Host "[WARN] Not running as Administrator, will try anyway." -ForegroundColor Yellow
    Write-Host "       If the copy fails, re-run in an elevated PowerShell." -ForegroundColor Yellow
}

# Backup original file once (keep the original Keil DLL)
if (Test-Path -LiteralPath $TargetDll) {
    if (Test-Path -LiteralPath $BackupDll) {
        Write-Host "[SKIP] Backup already exists: $BackupDll"
    } else {
        Copy-Item -LiteralPath $TargetDll -Destination $BackupDll -Force
        Write-Host "[ OK ] Backup created: $BackupDll"
    }
} else {
    Write-Host "[WARN] Target file does not exist, a new one will be created." -ForegroundColor Yellow
}

# Overwrite
try {
    Copy-Item -LiteralPath $SourceDll -Destination $TargetDll -Force
    Write-Host "[ OK ] Copied to $TargetDll" -ForegroundColor Green
} catch {
    Write-Host "[ERROR] Copy failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "        Close Keil uVision and make sure the DLL is not in use." -ForegroundColor Red
    exit 1
}

# Verify
$SrcHash = (Get-FileHash -LiteralPath $SourceDll -Algorithm SHA256).Hash
$DstHash = (Get-FileHash -LiteralPath $TargetDll -Algorithm SHA256).Hash
Write-Host ""
if ($SrcHash -eq $DstHash) {
    Write-Host "========================================"
    Write-Host "Deploy SUCCESSFUL!" -ForegroundColor Green
    Write-Host "Target: $TargetDll"
    Write-Host "SHA256: $DstHash"
    Write-Host "Restore: Copy-Item '$BackupDll' '$TargetDll' -Force"
    Write-Host "========================================"
} else {
    Write-Host "[ERROR] Hash mismatch after copy!" -ForegroundColor Red
    Write-Host "  Source: $SrcHash"
    Write-Host "  Target: $DstHash"
    exit 1
}
