# IO Monitor 更新日志

本文档记录了 IO Monitor 项目的所有重要变更。

---

## [v1.6.0]

### Added

- **IOPS 与队列深度监控** — 新增 `IopsMonitor` 独立子页面模块，集成至高级监视页面（悬浮窗）框架
  - 通过 PDH 查询 `\PhysicalDisk(_Total)\Disk Reads/sec`、`\PhysicalDisk(_Total)\Disk Writes/sec` 获取实时 IOPS 数据
  - 通过 `\PhysicalDisk(_Total)\Current Disk Queue Length` 获取磁盘队列深度
  - 悬浮窗新增 "IOPS & Queue" 显示区域，展示总 IOPS（含读/写细分）及队列深度
  - 队列深度采用颜色编码：绿色（<1）、黄色（1-5）、红色（>5），直观反映磁盘压力
- **资源优化机制** — 高级监视模式（悬浮窗）关闭时，IOPS 监视线程完全停止并销毁
  - `IopsMonitor::stop()` 依次关闭线程句柄、移除 PDH 计数器、关闭 PDH 查询，确保零后台轮询
  - `IopsMonitor::start()` 仅在悬浮窗激活时动态初始化，采样间隔 500ms，最低优先级线程
- **悬浮窗界面重构** — 重新设计为三个清晰区域：吞吐量（Throughput）、IOPS 与队列深度、累计字节数

### Changed

- `OverlaySnapshot` 结构体新增 `readIops`、`writeIops`、`totalIops`、`queueDepth`、`iopsValid` 字段
- `OverlayWindow` 悬浮窗尺寸从 280×130 扩大为 300×200，以容纳新增数据区域
- `OverlayWindow::start()` 在创建悬浮窗前启动 IOPS 监控，失败时自动回滚
- `OverlayWindow::stop()` 在销毁悬浮窗后立即停止 IOPS 监控线程

---

## [v1.5.0]

### 新增功能

- **核心监视引擎** — 实现基于 `CreateToolhelp32Snapshot` + `GetProcessIoCounters` 的进程级 I/O 实时数据采集
- **物理磁盘统计** — 通过 PDH 性能计数器查询 `\PhysicalDisk(_Total)\Disk Read/Write Bytes/sec`
- **彩色控制台界面** — 基于 VT100/ANSI 转义序列的完整 TUI，含 box-drawing 字符边框、彩色高亮、差分更新
- **多维度排序**
  - 按总速率（读+写）排序 — 键 `T`
  - 按读取速率排序 — 键 `R`
  - 按写入速率排序 — 键 `W`
  - 按会话累计 I/O 排序 — 键 `S`
  - 按进程生命周期总 I/O 排序 — 键 `P`
- **CSV 录制功能** — 支持将 I/O 数据实时导出为结构化 CSV 文件，含完整进程路径、速率和累计统计
  - 异步写入队列（最大 200 条缓存），独立写入线程，不阻塞主循环
  - 自动创建 `records/` 输出目录
  - 文件名格式：`IO_YYYYMMDD_HHMMSS.csv`
  - 支持启动时自动录制（`-o` 参数）和运行时切换（`O` 键）
- **悬浮窗模式** — 25% 透明度的置顶迷你窗口
  - 显示当前总 I/O 速率、磁盘使用百分比、录制状态、活跃进程数
  - 鼠标悬停时变为 80% 不透明度
  - 双击或按 `M` 返回完整控制台
  - 独立 Win32 窗口线程，使用 `WS_EX_LAYERED` 实现透明度控制
- **双行进程显示** — 每个进程显示两行：进程名 + 完整镜像路径
- **速率趋势指示** — 上行/下行箭头（↑/↓）标记 I/O 速率变化
- **活跃度彩色编码** — 红色（>10MB/s）、黄色（>1MB/s）、绿色（>0）、灰色（空闲）
- **PID 重用检测** — 自动检测 PID 被回收的情况，重置历史计数
- **命令行参数** — 支持 `-s`/`-r`/`-n`/`-o`/`-h` 参数配置

### 性能优化

- 监视线程以 `THREAD_PRIORITY_LOWEST` + `THREAD_MODE_BACKGROUND_BEGIN` 运行
- 控制台界面使用行级缓存（row cache）实现差分更新，仅重绘变化行
- 定期清理已退出进程的历史数据（每 60 个采样周期）
- 可执行文件约 214KB，内存占用约 2-3MB

### 测试

- 建立完整 Python 自动化测试套件，共 **126 项测试用例**，通过率 100%
- **单元测试（106 项）**
  - `test_monitor.py`（32 项）：ProcessIOData 结构体行为、排序逻辑、采样间隔、PID 重用检测、并发安全性
  - `test_formatting.py`（50 项）：速率格式化、字节格式化、时间格式化、字符串截断、CSV 转义、UTF-8 转换
  - `test_recorder.py`（24 项）：CSV 文件创建、队列操作、写入线程生命周期、数据完整性
- **集成测试（20 项）**
  - `test_integration.py`：Monitor + Recorder 协作、全流程数据采集排序录制、异常恢复
- **测试基础设施**
  - `pytest.ini` — pytest 全局配置，含 7 种自定义 markers
  - `conftest.py` — 全局 fixtures、环境诊断、失败时自动收集系统信息
  - `test_runner.py` — 自动化测试运行器，支持并行执行、覆盖率收集
  - `run_tests.bat` — Windows 一键测试脚本
- **报告系统** — 自动生成 HTML（可视化）、JSON（结构化）、Markdown（文本摘要）三种格式报告

### 构建系统

- **CMake** — 支持 MSVC 和 MinGW-w64 编译器
  - 自动配置 `pdh`、`psapi`、`gdi32` 链接库
  - Release 模式启用 `/O2 /GL` + `/LTCG` 全程序优化
  - 设置 `WIN32_LEAN_AND_MEAN` 和 `NOMINMAX` 减小编译体积
- **Visual Studio** — 提供 `.slnx` 解决方案文件

### 兼容性

- 操作系统：Windows 10 版本 1607 及以上（需 VT100 终端支持）
- 编译器：MSVC 2019+ / MinGW-w64 8.0+
- C++ 标准：C++17
- Python 测试环境：Python 3.7+ / pytest 7.4+

---

## 版本命名规则

本项目遵循语义化版本（Semantic Versioning）规范：

- **主版本号（MAJOR）**：不兼容的 API 修改或重大架构变更
- **次版本号（MINOR）**：向下兼容的功能新增
- **修订号（PATCH）**：向下兼容的问题修复

---

## 图例

| 标记 | 含义 |
|------|------|
| 🚀 | 新增功能 |
| ⚡ | 性能优化 |
| 🐛 | Bug 修复 |
| 📝 | 文档更新 |
| 🧪 | 测试相关 |
| 🔧 | 构建/工具链变更 |
| ⚠️ | 不兼容变更 |
