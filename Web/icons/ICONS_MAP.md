# Quant_Sev 导航栏图标映射

## 图标选择对照表

| 导航项 | 当前图标文件 | 原始图标 (PyQt-Fluent-Widgets) | 图标含义 | 图标预览 |
|--------|-------------|-------------------------------|---------|---------|
| **账户管理** | `account.svg` | `People_black.svg` | 用户/团队管理 | 👥 |
| **交易界面** | `trade.svg` | `Market_black.svg` | 市场/交易所 | 📊 |
| **量化策略** | `strategy.svg` | `DeveloperTools_black.svg` | 开发工具/策略 | 🛠️ |
| **本地套利** | `arbitrage.svg` | `Sync_black.svg` | 同步/循环套利 | 🔄 |
| **AI策略+** | `ai-strategy.svg` | `Robot_black.svg` | 机器人/AI智能 |  |
| **历史回测** | `backtest.svg` | `History_black.svg` | 历史记录/回测 | 📜 |
| **交易编写** | `editor.svg` | `Code_black.svg` | 代码/编程编辑 | 💻 |
| **数据管理** | `data.svg` | `Folder_black.svg` | 文件夹/数据存储 | 📁 |
| **设置** | `settings.svg` | `Setting_black.svg` | 齿轮/系统设置 | ️ |

## 图标特点

✅ **专业图标库**: 来自微软官方 Fluent UI 设计体系  
✅ **矢量格式**: SVG 格式，无限缩放不失真  
✅ **统一风格**: 所有图标保持一致的设计语言  
✅ **主题适配**: 通过 CSS filter 自动适配界面主题色  
✅ **易于替换**: 可直接修改 SVG 文件或更换图标文件  

## 原始图标位置

所有原始图标位于: `C:\PyQt-Fluent-Widgets-Gallery\icons\`

- **黑色版本**: `*_black.svg` (用于亮色主题)
- **白色版本**: `*_white.svg` (用于暗色主题)

## 扩展图标

图标库中还包含 200+ 其他图标，可用于未来功能扩展：

- 交易相关: `Download.svg`, `Upload.svg`, `Save.svg`, `Print.svg`
- 图表相关: `PieSingle.svg`, `View.svg`, `Zoom.svg`, `Filter.svg`
- 通讯相关: `Mail.svg`, `Chat.svg`, `Phone.svg`, `Send.svg`
- 系统相关: `Setting.svg`, `Help.svg`, `Info.svg`, `Update.svg`
- 媒体相关: `Camera.svg`, `Photo.svg`, `Video.svg`, `Music.svg`

## 使用方法

```html
<!-- 在 HTML 中使用 -->
<img class="nav-icon" src="icons/account.svg" alt="账户管理">
```

```css
/* CSS 样式控制 */
.nav-icon {
    width: 16px;
    height: 16px;
    filter: brightness(0) saturate(100%) invert(58%) sepia(65%) saturate(447%) hue-rotate(171deg);
}
```

## 注意事项

1. 当前使用黑色版本图标 + CSS filter 实现蓝色效果
2. 如需支持亮/暗主题切换，可动态切换 `_black` 和 `_white` 版本
3. 所有图标已统一命名，便于管理和维护
4. 建议保留原始图标文件，方便后续替换和扩展
