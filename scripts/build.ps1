# Quant_Sev 构建脚本（VS2026 / MinGW64）
param(
    [ValidateSet("msvc", "mingw")]
    [string]$Toolchain = "msvc",
    [switch]$EnableCtp,
    [switch]$DisableCtp,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

$cmakeVs = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
$cmake = if (Test-Path $cmakeVs) { $cmakeVs } else { "cmake" }

$ctpLibsPresent = (Test-Path "$Root\CTP\lib\thostmduserapi_se.lib") -and (Test-Path "$Root\CTP\lib\thosttraderapi_se.lib")
if ($DisableCtp) {
    $ctpFlag = "-DQUANT_SEV_ENABLE_CTP=OFF"
} elseif ($EnableCtp -or $ctpLibsPresent) {
    $ctpFlag = "-DQUANT_SEV_ENABLE_CTP=ON"
} else {
    $ctpFlag = "-DQUANT_SEV_ENABLE_CTP=OFF"
}

if ($Toolchain -eq "msvc") {
    $buildDir = "build-msvc"
    & $cmake -B $buildDir -S . -G "Visual Studio 18 2026" -A x64 $ctpFlag
    & $cmake --build $buildDir --config $Config --target quant_sev_host
    $exe = Join-Path $Root "$buildDir\bin\$Config\quant_sev_host.exe"
    if (-not (Test-Path $exe)) { $exe = Join-Path $Root "$buildDir\bin\quant_sev_host.exe" }
} else {
    $buildDir = "build-mingw"
    $env:Path = "C:\mingw64\bin;$env:Path"
    & $cmake -B $buildDir -S . -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=$Config $ctpFlag
    & $cmake --build $buildDir --target quant_sev_host
    $exe = Join-Path $Root "$buildDir\bin\quant_sev_host.exe"
}

Write-Host ""
Write-Host "构建完成: $exe" -ForegroundColor Green
Write-Host "CTP: $ctpFlag" -ForegroundColor DarkGray
Write-Host "运行: $exe $Root" -ForegroundColor Cyan
Write-Host "HTTP:  http://127.0.0.1:8080/Mainwindow.html" -ForegroundColor Cyan
Write-Host "WS:    ws://127.0.0.1:8081/ws/tick" -ForegroundColor Cyan
