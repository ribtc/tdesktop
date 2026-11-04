# 增强版页面样式核对

本次以仓库中的默认浅色主题和原生控件为基准，调整 `untitled.pen` 中的增强版页面。没有修改应用源码或运行构建。

## 代码基准

| 项目 | 代码定义 | 画布调整 |
| --- | --- | --- |
| 主按钮 | `activeButtonBg` → `windowBgActive`，`#40A7E3` | 替换自定义深蓝色 |
| 链接和活动文字 | `windowActiveTextFg`，`#168ACD` | 与按钮填充色分开使用 |
| 背景和分组 | `windowBg`、`windowBgOver`，白色和 `#F1F1F1` | 移除偏蓝的页面底色和重复圆角卡片 |
| 文字 | `windowFg`、`windowBoldFg`、`windowSubTextFg` | 替换自定义蓝灰色文字 |
| 字体和字号 | 默认字体 Open Sans；`fsize` 为 13px；`boxFontSize` 为 14px；表格值为 12px | 使用独立的 `code-font` 变量；11px 文字至少提高到 12px；编辑值采用 14px |
| 按钮 | `defaultActiveButton`、`defaultLightButton`，高度 34px；`buttonRadius` 为 4px | 统一高度；次要操作改为无边框文字按钮 |
| 输入框 | `defaultInputField`，底边 1px，焦点底边 2px，无圆角，最小高度 55px | 克隆来源和自定义签名改为底线输入；签名输入高度改为 55px |
| 复选框 | `defaultCheck`，22px，描边 2px | 克隆字段改用相同尺寸和描边 |
| 开关 | `defaultToggle`，滑块直径 16px，描边 2px | 替换实心胶囊加白色圆点的外观，采用细轨道和描边滑块 |
| 页签 | `defaultTabsSlider`，高度 43px，活动条 3px | 来源页签采用接近原生高度和 3px 活动条 |
| 分组分隔 | `boxDividerHeight`，8px | 保护设置使用白底设置分组和 8px 分隔带 |

定义来源：[颜色](../../Telegram/lib_ui/ui/colors.palette)、[基础样式](../../Telegram/lib_ui/ui/basic.style)、[控件样式](../../Telegram/lib_ui/ui/widgets/widgets.style)、[设置样式](../../Telegram/SourceFiles/settings/settings.style)、[Info 样式](../../Telegram/SourceFiles/info/info.style)、[字体初始化](../../Telegram/lib_ui/ui/style/style_core.cpp)。

## 页面调整

| 页面 | 调整 |
| --- | --- |
| 账号管理 | 共用管理导航；统一页头、按钮、选中态、表头和文字层级 |
| 代理与隔离 | 按参考图采用代理管理概览、操作与搜索工具栏、完整列数据表、分页和分配统计；补齐剪贴板导入与逐条账号数量。保留独立列表状态，并展示带格式说明、TXT 上传、地址族选择、预览和确认操作的导入侧栏打开状态 |
| 保护设置 | 移除卡片式后台布局；统一设置分组、状态区和开关 |
| 未读预览与撤回提示 | 统一增强入口及页面中的辅助文字和操作颜色 |
| 批量资料：克隆账号 | 原生输入、22px 复选框、轻量取值切换和白底内容区 |
| 批量资料：文件导入 | 文件选择行改为底线形式，选择操作使用文字按钮 |
| 批量资料：执行结果 | 同步导航、按钮、文字及成功与失败状态色 |

管理导航原有 208px 和 248px 两套宽度，现统一为 208px，并使用 `D0BYum` 的实例。这是增强功能的设计约定，并非代码中已有的管理导航宽度。页面保留原有信息和业务行为。

## 验证和边界

- 检查页面及控件截图，核对对齐、间距、文字层级、选中状态和分隔方式。
- 对 7 张增强版页面检查节点边界，确认可见内容无裁切或溢出。
- 新增的 `code-*` 样式变量与旧变量隔离，避免影响其他页面。
- 画布图标是设计示意，落地时使用项目的 `st::icon` 资源。中文显示受字体回退影响，实际应用由 Qt 和系统字体决定；自定义字体、深色主题和界面缩放应由现有样式机制处理。
- 图稿验证不替代实际应用的像素对比或交互验证。
