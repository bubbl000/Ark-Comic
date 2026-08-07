# Ark Comic — 方舟漫画阅读器

一个干净且快速的 Windows 漫画阅读软件
漫画管理+漫画阅读一体
CBZ/ZIP 漫画仓库管理与阅读。

## 软件功能

### 漫画仓库管理（管理器）
- **三栏管理器界面**：左栏分类树 / 中栏卡片墙 / 右栏详情面板
- 书库存储：SQLite 数据库（漫画条目、进度、封面）
- 压缩包导入与扫描（CBZ / ZIP）

### 漫画阅读
- 阅读器窗口：加载 CBZ/ZIP 漫画，逐页阅读
- 图片解码：JPEG 走 **turbojpeg 加速**，有 N 卡时走 **nvJPEG 硬件解码**（自动回退 CPU）
- 其他格式（PNG/BMP 等）走 Windows WIC 解码

### 系统集成
- 托盘图标（最小化到托盘，右键菜单：显示主窗口 / 退出）
- 设置窗口（阅读进度记忆、托盘行为等）

## 参考来源

| 类别 | 参考 |
|---|---|
| **UI/交互设计** | C# 旧版「方舟漫画」（WPF）：深色主题 + 柠檬绿（#CBE93A）强调色、三栏管理器、阅读器布局——**视觉风格与布局照搬，代码为全新 C++20 重写**（不兼容旧版书库）|
| **工程体系** | Ark Viewer 2（同技术栈 Win32 + Direct2D，沿用其构建/打包/图标体系）|
| **解码库** | libjpeg-turbo（turbojpeg.dll）、NVIDIA nvJPEG（N 卡硬解）、miniz（ZIP 解压，单文件库）、SQLite（书库）|
| **系统 API** | Win32 / Direct2D / WIC / DWM（圆角窗口）|

## 开源软件与许可

| 库 | 用途 | 许可证 |
|---|---|---|
| [libjpeg-turbo](https://github.com/libjpeg-turbo/libjpeg-turbo)（turbojpeg.dll）| JPEG 解码加速（DCT 降采样）| BSD-3-Clause / IJG |
| [miniz](https://github.com/richgel999/miniz) | CBZ/ZIP 解压（单文件库，编译进 exe）| Unlicense / MIT |
| [SQLite](https://www.sqlite.org/)（sqlite3）| 书库存储 | Public Domain |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON 解析（配置）| MIT |
| NVIDIA nvJPEG（nvjpeg64_12.dll）| N 卡 JPEG 硬件解码（运行时动态加载）| NVIDIA 专有（非开源，仅随安装包分发）|

> 说明：turbojpeg/nvjpeg/cudart 为二进制 DLL（运行时动态加载），未随源码仓库分发——构建前需放入 `third_party/bin/`（见构建说明）或从发布包提取。

## AI 与人工分工

| 角色 | 负责内容 |
|---|---|
| **人工（用户）** | 产品设计、交互逻辑定义、需求文档（需求话术 01-06）、界面验收 |
| **AI 编码助手（Trae）** | 全部源码实现、UI 自绘、功能编码 |
| **AI 辅助（Hermes）** | 构建打包、问题定位、性能调试、发布验证 |

> 说明：软件的需求与验收由人工完成，代码由 AI 编程助手实现——**交互逻辑以人工定义为准**。

## 构建

```
build.bat
```

- 需要：Visual Studio 2026（MSVC x64）+ CMake + Ninja
- 全量构建（清空 build 重配），构建后自动拷贝运行时 DLL（turbojpeg/nvjpeg/cudart，来自 `third_party/bin/`——该目录不在仓库内，构建前需自行放入或从发布包提取）

## 系统要求

- Windows 10 / 11（x64）
- 可选：NVIDIA 显卡（nvJPEG 硬解加速，无 N 卡自动回退 CPU）

## 打包发布

```
packaging\ArkComic_setup.iss  （Inno Setup 6）
→ release_pkg\ArkComic_Setup.exe
```

## 许可证

GPL-3.0（详见 LICENSE）
