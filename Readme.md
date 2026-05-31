# Quant_Sev

期货量化交易系统。

| 文档 | 角色 |
|------|------|
| **[Quant_Sev_Sod.md](Quant_Sev_Sod.md)** | 流程图与设计准绳（v1.5，只读参考） |
| **[frame.md](frame.md)** | 系统结构、模块、目录、API、依赖 |
| **[plan.md](plan.md)** | 开发进度与 Phase 任务 |

## 仓库结构

```
Quant_Sev/
├── frame.md             # 系统框架结构
├── plan.md              # 开发计划与进度
├── Quant_Sev_Sod.md     # 架构与流程图（只读准绳）
├── CMakeLists.txt       # 构建入口
├── config/              # 运行时 JSON 配置
├── data/                # Storage 数据根目录
├── CTP/                 # 官方 CTP SDK 头文件 → Readme.md
├── Core/                # 业务层（Gateway/CTA/Quote/Trade…）→ Readme.md
├── BLL/                 # 逻辑层（Bar/Storage/Strategy…）→ Readme.md
├── Host/                # HTTP/WS/Static 服务 → Readme.md
└── Web/                 # Web UI + Bridge 脚本 → Readme.md
```

## 快速链接

| 文档 | 说明 |
|------|------|
| [frame.md](frame.md) | 模块注册表、数据路径、API 面 |
| [plan.md](plan.md) | Phase 0–5 开发进程 |
| [Quant_Sev_Sod.md](Quant_Sev_Sod.md) | 全系统流程图 v1.5 |
| [Core/Readme.md](Core/Readme.md) | Core 模块与管控链 |
| [BLL/Readme.md](BLL/Readme.md) | 行情驱动与策略 |
| [Host/Readme.md](Host/Readme.md) | Host 与 Bridge |
| [Web/Readme.md](Web/Readme.md) | UI 与 Gateway 数据路径 |
| [CTP/README.md](CTP/README.md) | CTP API 参考 |

## 本地运行（Phase 2）

已验证工具链：**VS2026 (MSVC 19.51)**、**MinGW64 (GCC 16.1)**。

### 一键构建

```powershell
# VS2026（推荐）
.\scripts\build.ps1 -Toolchain msvc -Config Release

# MinGW64
.\scripts\build.ps1 -Toolchain mingw -Config Release

# 启用 CTP（需先将 .lib/.dll 放入 CTP/lib/）
.\scripts\build.ps1 -Toolchain msvc -EnableCtp
```

输出：`build-msvc\bin\Release\quant_sev_host.exe` 或 `build-mingw\bin\quant_sev_host.exe`

### 手动 CMake

```powershell
# VS2026 自带 CMake 路径示例
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake -B build-msvc -S . -G "Visual Studio 18 2026" -A x64
& $cmake --build build-msvc --config Release --target quant_sev_host
```

```powershell
# MinGW64
$env:Path = "C:\mingw64\bin;$env:Path"
cmake -B build-mingw -S . -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build-mingw --target quant_sev_host
```

### 运行

```powershell
.\build-msvc\bin\Release\quant_sev_host.exe c:\dev\Quant_Sev
```

浏览器打开 `http://127.0.0.1:8080/Mainwindow.html`。

依赖头文件已 vendored 至 `third_party/`（无需联网 FetchContent）。
