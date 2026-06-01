# Quant_Sev 可移植运行包打包脚本
param(
    [ValidateSet("msvc", "mingw")]
    [string]$Toolchain = "msvc",
    [string]$Config = "Release",
    [string]$OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

& "$Root\scripts\build.ps1" -Toolchain $Toolchain -Config $Config

$dest = Join-Path $Root $OutDir
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
New-Item -ItemType Directory -Path $dest | Out-Null

$items = @(
    "config",
    "Web",
    "CTP",
    "data",
    "scripts\build.ps1",
    "scripts\package.ps1",
    "CMakeLists.txt",
    "Readme.md"
)
foreach ($item in $items) {
    $src = Join-Path $Root $item
    if (Test-Path $src) {
        Copy-Item -Recurse -Force $src (Join-Path $dest (Split-Path $item -Leaf))
    }
}

if (-not (Test-Path (Join-Path $dest "config\Account.json")) -and (Test-Path (Join-Path $Root "config\Account.json"))) {
    Copy-Item (Join-Path $Root "config\Account.json") (Join-Path $dest "config\Account.json")
}
if (-not (Test-Path (Join-Path $dest "config\Account.json"))) {
    Copy-Item (Join-Path $Root "config\Account.json.example") (Join-Path $dest "config\Account.json")
}

if ($Toolchain -eq "msvc") {
    $exeSrc = Join-Path $Root "build-msvc\bin\$Config\quant_sev_host.exe"
    if (-not (Test-Path $exeSrc)) { $exeSrc = Join-Path $Root "build-msvc\bin\quant_sev_host.exe" }
} else {
    $exeSrc = Join-Path $Root "build-mingw\bin\quant_sev_host.exe"
    $mingwBin = "C:\mingw64\bin"
    foreach ($dll in @("libgcc_s_seh-1.dll", "libstdc++-6.dll", "libwinpthread-1.dll")) {
        $dllPath = Join-Path $mingwBin $dll
        if (Test-Path $dllPath) {
            Copy-Item $dllPath $dest
        }
    }
}

Copy-Item $exeSrc (Join-Path $dest "quant_sev_host.exe")

$runBat = @"
@echo off
cd /d "%~dp0"
quant_sev_host.exe "%~dp0"
pause
"@
Set-Content -Path (Join-Path $dest "run.bat") -Value $runBat -Encoding ASCII

Write-Host ""
Write-Host "打包完成: $dest" -ForegroundColor Green
Write-Host "将整个 dist 文件夹复制到新电脑，双击 run.bat 启动。" -ForegroundColor Cyan
Write-Host "MSVC 版需安装 Visual C++ 运行库: https://aka.ms/vs/17/release/vc_redist.x64.exe" -ForegroundColor Yellow
