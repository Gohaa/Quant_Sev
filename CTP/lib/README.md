# CTP 运行时库目录

将 SimNow / 期货公司提供的 **导入库与 DLL** 放入本目录后再开启 CTP 编译：

| 文件（Windows 示例） | 用途 |
|----------------------|------|
| `thostmduserapi_se.lib` + `thostmduserapi_se.dll` | 行情 |
| `thosttraderapi_se.lib` + `thosttraderapi_se.dll` | 交易 |

构建：

```bash
cmake -B build -S . -DQUANT_SEV_ENABLE_CTP=ON
cmake --build build --config Release
```

运行前将对应 `.dll` 复制到 `quant_sev_host.exe` 同目录，或加入 `PATH`。

头文件位于上级目录 `CTP/`（仓库已含，禁止修改）。
