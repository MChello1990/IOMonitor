# IO Monitor 更新日志

本文档记录了 IO Monitor 项目的所有重要变更。

---

## [v2.0.0] — 2026-07-24

###  Bug 修复

- **SMART 数据模型修复** — `NvmeIdCtrl` 结构体大小修正为 4096 字节（NVMe 规范），`NvmeSmartLog` 512 字节静态断言
- **编译兼容性** — 移除对未定义 SDK 类型 `STORAGE_ADDITIONAL_PARAMETERS` 的依赖，改用标准 `FIELD_OFFSET` + `memcpy` 模式
- **多磁盘健康评分** — 修复 NVMe 磁盘健康直接归零问题：ATA 保留 SMART RETURN STATUS 立即失败判定，NVMe Critical Warning 改为通过加权属性系统（`NVME_CRITICAL_WARNING` 权重）处理
- **温度显示** — 修复 NVMe 磁盘温度错误（Kelvin 低字节被误作 Celsius），改为解析 rawString 中已转换的 Celsius 值
- **累计 I/O 统计** — 修复 NVMe 磁盘 Data Units Read/Written 数据量显示错误（修正乘数：512 → 512,000，NVMe 规范定义每 data unit = 512×1000 字节）
- **窗口重新打开** — 修复关闭 SMART 窗口后无法再次打开的问题（上次会话的 WM_QUIT 残留在线程消息队列中，导致新窗口立即退出）
- **缓冲区偏移** — 修复 `readSmartLogViaStorageQuery` 中响应数据读取偏移量 4 字节错位（`sizeof(STORAGE_PROPERTY_QUERY)` 含尾部填充，改用 `FIELD_OFFSET` 定位 `AdditionalParameters`）
- **函数链接** — 修复 `SmartMonitor::createReaderForDisk` 被错误定义为文件静态函数，以及孤立的非法代码块
- **缺失实现** — 添加 `SmartReaderAta::checkSmartStatus()` 实现（ATA SMART RETURN STATUS 透传命令）
- **项目编译** — 将 `SmartDataModel.cpp` 加入 VS 项目编译列表，修复 9 个链接错误
- **编码损坏** — 修复源代码中多处 Unicode 编码损坏字符和分节分隔符

### 改进

- **NVMe 温度采集** — 改用 `diskInterface` 检测替代 `temperatureCelsius == 0.0` 条件，确保 NVMe 温度始终正确提取
- **消息队列清理** — `show()` 启动时使用 `PeekMessageW(PM_REMOVE)` 排空残留消息，防止跨会话消息污染
- **总线检测内联** — `createReaderForDisk` 改为成员函数，内联实现总线类型检测和读取器创建（移除对工厂函数的冗余委托）
- **GDI 渲染稳定性** — 修正 `paintOverlay` 等函数中的 GDI 对象管理模式

### 性能

- 用户体验：SMART 窗口关闭后可正常重新打开，无需重启程序
- NVMe 磁盘数据统计准确度修复，Total I/O 显示值正确反映实际数据量

---

## [v1.7.0] — 2026-07-24

###  新增功能

- **SMART 硬盘健康监控** — 全新的独立监控页面，基于 smartmontools 设计理念实现
  - **多协议支持** — 统一的抽象读取器接口 `SmartReaderBase`，支持三种磁盘协议：
    - `SmartReaderAta` — ATA/SATA 磁盘，通过 IOCTL `SMART_RCV_DRIVE_DATA` / `SMART_CMD` 透传 SFF-8035i 标准 ATA SMART 命令
    - `SmartReaderNvme` — NVMe 磁盘，通过 IOCTL `NVME_PASS_THROUGH` 发送 Admin Command `Get Log Page` (LID=0x02) 获取 SMART/Health Information Log
    - `SmartReaderScsi` — SCSI/SAS/USB 桥接磁盘，支持 Log Sense 4Dh 和 ATA PASS-THROUGH 两种方式
  - **完整数据模型** — 严格镜像 smartmontools 的 `atacmds.h`/`nvmecmds.h` 结构体定义：
    - ATA SMART 属性（`AtaSmartAttribute`）、阈值（`AtaSmartThresholds`）、IDENTIFY DEVICE（`AtaIdentifyDevice`）均为 512 字节 packed struct
    - NVMe SMART/Health Information Log（`NvmeSmartLog`）和 Identify Controller（`NvmeIdCtrl`）完全符合 NVMe Base Spec 2.0a
    - `AttrRawFormat` 枚举完整支持 19 种 raw value 解析格式（与 smartmontools 的 `ata_attr_raw_format` 一致）
    - `AttrFlags` 位掩码标志（INCREASING、NO_NORMVAL、NO_WORSTVAL、HDD_ONLY、SSD_ONLY）
  - **硬盘健康评分** — 基于关键 SMART 属性加权计算健康百分比：
    - ATA 指标权重：重映射扇区 25%、当前待映射 20%、离线不可纠正 20%、重映射事件 5%、Spin Retry 5% 等
    - NVMe 指标权重：Media Errors 20%、Critical Warning 30%、Percentage Used 20%、温度 10%、不安全关机 10%
    - SSD 专属指标：磨损均衡次数 15%、剩余寿命 20%、擦除失败 10%、编程失败 10%
    - 健康等级划分：优秀（≥90%）、良好（≥70%）、警告（≥50%）、危险（<50%）
  - **SMART 属性解析** — 完整映射 256 个 ATA SMART 属性 ID，含 smartmontools 默认属性名称
    - 属性状态判断（`AttrState`）：NON_EXISTING / NO_NORMVAL / NO_THRESHOLD / OK / FAILED_PAST / FAILED_NOW
    - Raw value 格式化引擎：支持 RAW48、HEX48、RAW64、HEX64、温度格式等多种格式
    - SMART RETURN STATUS 检查（`ataSmartStatus2()` 等效），返回值 0=正常、1=阈值超限、-1=不支持/错误
- **可视化 SMART 监控 GUI** — 基于 Win32 GDI+ 的独立窗口化监控页面
  - **磁盘选择器** — 顶部下拉列表，自动枚举所有物理磁盘并标注接口类型（ATA/NVMe/SCSI）
  - **概览仪表盘** — 环形仪表盘显示健康百分比（颜色编码：绿/黄/橙/红）
  - **指标卡片** — 6 格卡片展示温度、通电时长、读/写速率、累计读/写字节数
  - **历史趋势图** — 迷你折线图（温度变化）和迷你柱状图（健康历史），支持拖拽查看
  - **详细属性面板** — 右侧可滚动列表，显示每个 SMART 属性的 ID、名称、当前值、最差值、阈值、Raw Value
    - 预失败属性（Pre-failure）红色高亮警告
    - 在线属性（Online）蓝色标记
    - 超过阈值的属性黄色背景标记
  - **状态栏** — 底部显示上次刷新时间、数据有效性、SMART 状态摘要
- **悬浮窗模式** — 精简的 SMART 健康迷你窗口
  - 显示当前选中磁盘的健康百分比、温度、读/写速率
  - 健康状态彩色光环指示
  - 透明度和置顶行为与 I/O 悬浮窗一致（25% 默认、鼠标悬停 80%）
- **调试追踪系统** — 线程感知的诊断日志框架（`SmartDebug.h`）
  - 环形缓冲区（8192 条），线程安全快照读取
  - 12 种追踪类别：线程生命周期、磁盘发现、IOCTL 调用、数据刷新、健康计算、速率计算、属性解析、窗口事件、悬浮窗事件、用户交互、错误异常
  - RAII 作用域追踪宏 `SMART_TRACE_SCOPE`，自动配对 begin/end 并计算耗时
  - 编译时开关 `SMART_DEBUG_ENABLED` 控制追踪开销
- **调试查看器窗口** — 实时追踪可视化工具（`SmartDebugWindow`）
  - **Tab 1: Trace Log** — 可滚动追踪条目列表，显示线程 ID、类别、时间戳、函数名、消息、耗时
    - 类别颜色编码（12 种颜色）
    - 自动滚动 / 暂停（空格键切换）
    - Ctrl+L 切换文件日志记录
  - **Tab 2: Thread Stats** — 每线程运行时统计（条目数、错误数、总耗时、最大耗时、最后活动时间）
  - **Tab 3: Flow Graph** — ASCII 艺术时间线，展示主要操作的执行流程
  - F5 刷新、Ctrl+C 清空所有追踪数据
  - 文件日志输出至 `IO_SMART_Debug_YYYYMMDD_HHMMSS.log`
- **数据采集线程** — 独立低优先级刷新线程，默认 120 秒间隔
  - 增量速率计算（对比前后两次快照的 LBA 计数差 / 时间间隔）
  - 温度历史记录（`m_tempHistory` deque）用于趋势图
  - 健康百分比历史记录（`m_healthHistory` deque）用于健康趋势
- **工厂模式磁盘检测** — `createSmartReader()` 自动探测磁盘接口类型
  - 优先尝试 ATA IDENTIFY DEVICE，失败后尝试 NVMe Identify Controller，最后回退到 SCSI

### 构建系统

- `CMakeLists.txt` 新增 `setupapi` 链接库（磁盘枚举需要 `SetupDiGetClassDevs` 系列 API）
- 项目版本号更新为 `v1.7.0`
- 新增 11 个源文件（`SmartDataModel.cpp/.h`, `SmartReaderBase.cpp/.h`, `SmartReaderAta.cpp/.h`, `SmartReaderNvme.cpp/.h`, `SmartReaderScsi.cpp/.h`, `SmartMonitor.cpp/.h`, `SmartOverlayWindow.cpp/.h`, `SmartDebug.h`, `SmartDebugWindow.cpp/.h`）

---

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
