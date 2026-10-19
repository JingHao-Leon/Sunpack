# SunPack 管理面板

独立的 Windows C++17 / Core UI 管理器，产物为 `SunPackGUI.exe`。
界面资源、Core UI、QuickJS 和 JSON 库静态编入 EXE，使用静态 CRT。
GUI 关闭即退出，Watch 的生命周期独立于 GUI。
界面参照 Windows 11 设置：左侧导航和细竖条选中提示，28 px 页面标题、
8 px 圆角卡片和低对比度边框；字体使用 Segoe UI Variable Text / 微软雅黑 UI。
支持跟随系统、浅色和深色外观。字段和密码编辑使用独立内容区，最小窗口下
表单可滚动，保存按钮固定可见；侧栏的目录与密码来源也在独立滚动区域内。
系统标题栏同步深浅色，Windows 11 使用系统圆角；不支持这些 DWM 属性的
旧系统保留默认窗口边框，不加载额外视觉效果或后台定时器。

## 构建与运行

需要 Visual Studio 2022 C++ Build Tools、Windows SDK、CMake 3.25+、Git。
使用仓库已有 `.venv`；首次配置从固定上游提交获取依赖。

```powershell
.\scripts\build_gui.ps1 -Arch x64
# ARM64 需要相应 MSVC ARM64 编译组件；在原生 ARM64 runner 上也执行测试。
.\scripts\build_gui.ps1 -Arch arm64
```

开发构建在 `.cache/gui-<架构>/Release/SunPackGUI.exe`。从源码运行的托盘
使用相同位置，并传入仓库数据目录及现有 Python CLI 入口。

```powershell
# 独立数据目录默认禁用 CLI 控制，防止测试数据误操作已安装的 Watch。
.\.cache\gui-x64\Release\SunPackGUI.exe --data-dir C:\SunPackGuiFixture
# 显式选择用于控制的 CLI；可重复 --cli-prefix 传入固定启动参数。
.\.cache\gui-x64\Release\SunPackGUI.exe --data-dir C:\SunPackGuiFixture --cli C:\SunPack\bin\sunpack.exe
```

安装版默认读取 `%ProgramData%\SunPack`，CLI 为同一安装目录的
`bin\sunpack.exe`。按数据目录单实例，再次打开会激活已有窗口。

## 数据与交互

- 任务：读取当前 `events.jsonl`，只根据已记录事件投影状态。每页最多 100 行。
  保留字节偏移及未完成末行；比较文件身份、固定前缀和已读末端锚点来识别
  原文件截断后重新增长。重置会清除旧投影，读取不涉及分卷、密码或格式分析。
- 目录：读取和编辑 `sunpack_watch_roots.txt`，保留注释，最后一条重复目录生效；
  默认输出相对该输入目录解析。导航每次最多加载 50 个目录。
- 密码：严格校验 `builtin_passwords.json`；保留原始密码文本和剪贴板历史。
  显示直接、父目录继承及全局来源，支持多作用域编辑和显式转换为全局。
  移除最后一个作用域会删除记录。剪贴板历史只读，可复制为手动密码。
- 配置：只发现主配置已有字段；类型驱动编辑，数组、对象与 null 使用 JSON。
  保存时在锁内回读，只替换指定字段的原始 JSON token，因此未知字段、格式、
  数组结构和超过 JavaScript/uint64 精度的大整数均不被重新序列化损坏。
  同字段、同密码记录的外部冲突会提示用户重新选择。
- 文件编辑使用同目录临时文件和原子替换。密码、目录分别复用运行时的
  `Local\SunPack.EditableFile-<XXH64>`、`Local\SunPackWatchRoots-<XXH64>`。
  Windows 路径大小写通过 NT Unicode 映射，XXH64 复用已有 LZ4 源码。
- 使用 `ReadDirectoryChangesW` 和注册表变化通知；不扫描压缩文件或监控根目录。
  日志位置随实际主配置的 `watch.state_dir` 变化重新绑定。共享冲突等临时读取错误
  保留上一份有效数据，仅调度最多三次 5/20/80 ms 的单次重试，之后等待新通知
  或手动刷新；没有空闲轮询。目录不存在时观察现有祖先，创建后重新绑定。
- Watch 启停、状态、重载、自启开关仅启动现有 CLI 命令。自启显示值直接读取
  `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\SunPackWatchService`。
  CLI 输出以自动删除的临时句柄捕获，不保存额外历史，不启动 GUI 的 IPC 服务。
  资源管理器入口定位文件，避免执行伪装成 `.exe` 的压缩包。

Core UI 固定提交存在嵌套 `v-if` 子树卸载后的悬空观察器问题，已由 ASan 复现。
本界面保留视图容器并使用 `v-show`，仅顶层命令信号使用 `v-if`；该信号的
原生句柄在卸载时释放。列表仍分页，未为全部日志创建控件。窗口使用
prepare/show-immediate 路径，使用系统标题栏，不创建托盘或 Toast。

## 验证

`build_gui.ps1` 运行两个原生粗粒度测试及 Python 跨语言契约测试。
`tests/gui/README.md` 说明每个测试的独立覆盖和成本。
可设置 `SUNPACK_GUI_SCREENSHOTS` 为一个新目录来保存三个页面、浅色主题及
最小窗口编辑状态的测试截图。
内存诊断可配置 `-DSUNPACK_GUI_ASAN=ON`，不要发布 ASan 产物。

2026-10-18 本机 x64 Release 实测：EXE 约 3 MiB；一个目录、一个日志任务时
工作集约 53 MiB、私有内存约 56 MiB，3 秒空闲 CPU 时间增量为 0，关闭退出码 0。
这些数字包含系统图形驱动开销，随机器、DPI、目录及任务数量变化。

x64 Release、原生界面/文件契约、跨语言锁与剪贴板协作、ASan 界面测试已运行。
安装器的 x64、ARM64 编译检查已运行。当前本机没有 MSVC ARM64 编译组件，
ARM64 GUI 的实际构建与运行尚待双架构发布 CI 验证；旧 CPU、Windows 10 和
真实 ARM64 设备的运行体验也仍需设备验收。没有在本次开发中发布完整安装包。

发布脚本会打包 GUI 及依赖许可证，校验 PE 架构与 GUI 子系统，安装器提供
开始菜单入口，并在升级/卸载前关闭对应安装目录的 GUI。沿用现有用户数据策略。
