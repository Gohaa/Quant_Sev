# Quant_Sev Web 前端

本目录为 Quant_Sev 的 **Web UI 静态资源**，由 Host 服务层（HTTP / WebSocket / Static）托管，经 **Gateway** 与 Core/BLL 后端通信。入口页面为 `Mainwindow.html`。

> 架构对齐见 [`Quant_Sev_Sod.md`](../Quant_Sev_Sod.md) **§4.1**、**§5.1**。  
> 开发进度见 [`plan.md`](../plan.md)。

---

## 目录结构

```
Web/
├── Mainwindow.html          # 主壳：侧栏导航 + iframe 内容区 + 连接状态
├── Mainwindow.css           # 主壳样式
├── Account_ui.html          # 账户管理（加载/切换/登录账户）
├── Trade_ui.html            # 交易主界面（嵌入 trade/ 子组件 + Logger）
├── Risk_ui.html             # 风控监控与应急操作
├── Strategies_ui.html       # 量化策略启停与参数
├── FollowTrade_ui.html      # 跟单管理
├── AI_Strategy_ui.html      # AI 策略配置
├── Backtest_ui.html         # 历史回测
├── Editor_ui.html           # 策略/脚本编写（待完善）
├── Data_ui.html             # 本地数据管理（Storage 查询/导入）
├── Settings_ui.html         # 系统设置
├── Logger.html              # 运行日志（可独立打开或 iframe 嵌入）
├── trade/                   # 交易界面子组件（由 Trade_ui 以 iframe 加载）
│   ├── Market_Chart.html    # K 线 / 分时主图容器
│   ├── Market_Chart.js      # 图表逻辑（ECharts）
│   ├── Market_Chart.css
│   ├── Market_Chart_App.js  # 图表与 Bridge 绑定
│   ├── Trade_control.html   # 人工报单面板（买/卖/开平/价格/数量）
│   ├── Trade_Query.html     # 委托/成交/资金查询
│   ├── DOM_Panel.html       # 盘口深度
│   ├── DOM_Panel.js
│   ├── Position_Panel.html  # 持仓列表
│   ├── Position_Panel.js
│   ├── MACD_Chart.js        # MACD 副图
│   └── Chip_Chart.js        # 筹码分布副图
└── icons/                   # 侧栏 SVG 图标
```

### Host 托管的桥接脚本（与 HTML 同级部署）

以下文件由 **Host** 静态托管，与 HTML 同级部署（**Phase 1 已提供占位实现**）：

| 文件 | 作用 |
|------|------|
| `logger_Bridge.js` | 统一 HTTP 传输、`QuantSevBridge.Logger`、账户/配置 API、页面 `autoInitPage` |
| `tick_Bridge.js` | WebSocket 订阅 Tick，驱动分时/最新价 |
| `TradeView_Bridge.js` | 合约列表轮询、K 线/Bar 拉取、图表数据绑定 |
| `trade_Bridge.js` | 人工报单/撤单 HTTP 封装（→ Gateway → Trade） |
| `Strategies_Bridge.js` | 策略列表、启停、传参（→ Gateway → CTA） |
| `Backtest_Bridge.js` | 回测任务提交与进度（→ Gateway → Backtest Engine） |

全局命名空间：`window.QuantSevBridge`（HTTP 模式）或 `window.UICore`（嵌入式 C++ 桥，可选）。

---

## 页面与 Gateway 数据路径

| 页面 | 主要功能 | HTTP（REST） | WebSocket | 后端数据源 |
|------|----------|--------------|-----------|------------|
| `Mainwindow.html` | 导航壳、`/api/status` 行情/交易连接灯 | ✓ | — | Gateway |
| `Account_ui.html` | 账户增删、Gateway 触发 Account 加载 | `/api/saved_accounts` 等 | — | Config → Gateway → Trade |
| `Trade_ui.html` | 行情图 + 人工下单 + 查询/持仓 | 报单、查询、历史 Bar | Tick、K 线、CTA 资管推送 | Quote/Storage/CTA |
| `trade/Trade_control.html` | **人工发单入口** | 报单 → Gateway → **Trade**（不经 CTA 发信号） | — | Trade 回报 → Gateway → CTA → UI |
| `trade/Trade_Query.html` | 委托/成交/资金 | 查询 API | 推送更新 | **CTA**（资管视图） |
| `trade/Market_Chart.html` | K 线/Tick 图 | 历史 Bar、**实时指标 API**（→ IND 直连） | Tick/K 线 | Quote/Storage/IND |
| `Strategies_ui.html` | 策略启停/传参 | → Gateway → **CTA** | 策略状态 | CTA → Strategy |
| `Risk_ui.html` | 风控面板、急平/停策略 | → Gateway → CTA/Config | 告警 | CTA、Risk |
| `Backtest/Run.html` | 历史回测 | → Gateway → Backtest | 进度 | Match 模拟 Trade |
| `Backtest/Optimize.html` | 参数优化 | → Gateway → Backtest | 进度 | 网格搜索 |
| `Data_ui.html` | Storage 数据浏览/导入 | 历史 Tick/Bar | — | Storage |
| `Logger.html` | 系统日志 | `/api/ui_logs` | 可选实时 | Logger → Gateway |

### 两条发单入口（勿混淆）

1. **策略发单**：`Strategy → CTA → Gateway → Trade`（信号经 CTA 资管与风控）。
2. **人工发单**：`Trade_control / Trade_ui → HTTP → Gateway → Trade`（UI 直连 Gateway 报单，回报仍 **Trade → Gateway → CTA → UI**）。

### Indicator API 指标策略

| 模式 | 路径 |
|------|------|
| **实盘** | UI → HTTP → Gateway → CTA → STRATEGY → CTA → Gateway → WS/HTTP → UI（图表叠加指标策略信号） |
| **回测** | Backtest_ui → Gateway → CTA → STRATEGY；行情来自 Storage 回放，不经 Quote 实时链路 |

实时 Tulip 指标（非策略）：`HTTP → IND` 直连，刻意不经 Storage，低延迟；历史指标经 Storage 读取。

---

## 组件嵌套关系

```
Mainwindow.html
  └── iframe → Account_ui | Trade_ui | Risk_ui | …（侧栏切换）

Trade_ui.html
  ├── iframe → trade/Market_Chart.html   （tick_Bridge + TradeView_Bridge）
  ├── iframe → trade/Trade_Query.html
  ├── iframe → trade/DOM_Panel.html
  ├── iframe → trade/Trade_control.html  （人工报单）
  ├── iframe → trade/Position_Panel.html
  └── iframe → Logger.html?embed=1

Risk_ui.html / 多数业务页
  └── iframe → Logger.html?embed=1        （右侧运行日志）
```

---

## 本地预览

Host 启动后访问（端口以 Host 配置为准）：

```
http://127.0.0.1:<port>/Mainwindow.html
```

无 Host 时 HTML 可打开但 Bridge API/WebSocket 不可用；`QuantSevBridge` 未定义时页面保持静态展示。

---

## 开发约定

- 业务页统一引入：`logger_Bridge.js`；需行情/交易时再引入 `tick_Bridge.js`、`TradeView_Bridge.js`。
- 日志区使用 `<iframe src="Logger.html?embed=1">`，避免样式冲突。
- 新增页面须在 `Mainwindow.html` 侧栏注册 `data-page` 路径。
- 所有控制类操作（Account/Symbol/Quote/Trade 加载、策略启停）经 **HTTP → Gateway**，禁止前端直连 Trade/CTA 模块。
