# IO Monitor Changelog

This document records all notable changes to the IO Monitor project.

---

## [v2.0.0] — 2026-07-24

### 🐛 Bug Fixes

- **SMART Data Model** — Fixed `NvmeIdCtrl` struct size from 2048 to 4096 bytes (NVMe spec), `NvmeSmartLog` 512-byte static assertion
- **Build Compatibility** — Removed dependency on undefined SDK type `STORAGE_ADDITIONAL_PARAMETERS`, using standard `FIELD_OFFSET` + `memcpy` pattern
- **NVMe Health Scoring** — Fixed NVMe disk health incorrectly showing 0%: ATA retains immediate SMART RETURN STATUS failure, NVMe Critical Warning handled through weighted attribute system (`NVME_CRITICAL_WARNING` weight)
- **Temperature Display** — Fixed NVMe temperature error (Kelvin low byte was mistakenly used as Celsius); now parses the correctly-converted Celsius value from rawString
- **Total I/O Statistics** — Fixed NVMe Data Units Read/Written values showing 1/1000th of actual (multiplier corrected: 512 → 512,000, per NVMe spec: 1 data unit = 512×1000 bytes)
- **Window Reopen** — Fixed SMART window unable to restart after closing (stale WM_QUIT from previous session lingering in thread message queue)
- **Buffer Offset** — Fixed 4-byte misalignment in `readSmartLogViaStorageQuery` response data read offset (`sizeof(STORAGE_PROPERTY_QUERY)` includes tail padding; using `FIELD_OFFSET` to locate `AdditionalParameters`)
- **Function Linkage** — Fixed `SmartMonitor::createReaderForDisk` incorrectly defined as a file-static free function with orphaned illegal code blocks
- **Missing Implementation** — Added `SmartReaderAta::checkSmartStatus()` implementation (ATA SMART RETURN STATUS pass-through command)
- **Project Compilation** — Added `SmartDataModel.cpp` to VS project file, resolving 9 linker errors
- **Encoding Corruption** — Fixed multiple Unicode encoding corruption characters and section separators in source code

### 🔧 Improvements

- **NVMe Temperature Extraction** — Changed to use `diskInterface` detection instead of `temperatureCelsius == 0.0` condition, ensuring correct NVMe temperature extraction
- **Message Queue Cleanup** — `show()` now drains stale messages with `PeekMessageW(PM_REMOVE)` before starting the message loop
- **Bus Detection Inlining** — `createReaderForDisk` converted to member function with inline bus type detection and reader creation
- **GDI Rendering Stability** — Fixed GDI object management patterns in `paintOverlay` and other functions

### ⚡ Performance

- UX: SMART window can be reopened normally after closing, no program restart needed
- NVMe data statistics accuracy fixed; Total I/O values now correctly reflect actual data volume

---

## [v1.7.0] — 2026-07-24

### 🚀 Added

- **SMART Disk Health Monitoring** — A brand-new standalone monitoring page, built on smartmontools design principles
  - **Multi-Protocol Support** — Unified abstract reader interface `SmartReaderBase`, supporting three disk protocols:
    - `SmartReaderAta` — ATA/SATA disks, sending SFF-8035i standard ATA SMART commands via IOCTL `SMART_RCV_DRIVE_DATA` / `SMART_CMD` pass-through
    - `SmartReaderNvme` — NVMe disks, sending Admin Command `Get Log Page` (LID=0x02) for SMART/Health Information Log via IOCTL `NVME_PASS_THROUGH`
    - `SmartReaderScsi` — SCSI/SAS/USB-bridged disks, supporting Log Sense 4Dh and ATA PASS-THROUGH (SAT) methods
  - **Complete Data Model** — Strictly mirrors smartmontools' `atacmds.h`/`nvmecmds.h` structure definitions:
    - ATA SMART attributes (`AtaSmartAttribute`), thresholds (`AtaSmartThresholds`), IDENTIFY DEVICE (`AtaIdentifyDevice`) — all 512-byte packed structs
    - NVMe SMART/Health Information Log (`NvmeSmartLog`) and Identify Controller (`NvmeIdCtrl`) — fully compliant with NVMe Base Spec 2.0a
    - `AttrRawFormat` enum fully supports 19 raw value parsing formats (matching smartmontools' `ata_attr_raw_format`)
    - `AttrFlags` bitmask flags (INCREASING, NO_NORMVAL, NO_WORSTVAL, HDD_ONLY, SSD_ONLY)
  - **Disk Health Scoring** — Weighted health percentage calculation based on critical SMART attributes:
    - ATA indicator weights: Reallocated Sectors 25%, Current Pending 20%, Offline Uncorrectable 20%, Reallocated Events 5%, Spin Retry 5%, etc.
    - NVMe indicator weights: Media Errors 20%, Critical Warning 30%, Percentage Used 20%, Temperature 10%, Unsafe Shutdowns 10%
    - SSD-specific indicators: Wear Leveling 15%, Remaining Life 20%, Erase Fail 10%, Program Fail 10%
    - Health grades: Excellent (≥90%), Good (≥70%), Warning (≥50%), Critical (<50%)
  - **SMART Attribute Parsing** — Full mapping of 256 ATA SMART attribute IDs with smartmontools default attribute names
    - Attribute state detection (`AttrState`): NON_EXISTING / NO_NORMVAL / NO_THRESHOLD / OK / FAILED_PAST / FAILED_NOW
    - Raw value formatting engine: supports RAW48, HEX48, RAW64, HEX64, temperature formats, and more
    - SMART RETURN STATUS check (`ataSmartStatus2()` equivalent), returning 0=good, 1=threshold exceeded, -1=unsupported/error
- **Visual SMART Monitoring GUI** — Independent Win32 GDI+ window-based monitoring page
  - **Disk Selector** — Top dropdown list, auto-enumerates all physical disks with interface type labels (ATA/NVMe/SCSI)
  - **Overview Dashboard** — Ring gauge showing health percentage (color-coded: green/yellow/orange/red)
  - **Metric Cards** — 6-card grid displaying temperature, power-on hours, read/write rates, cumulative read/write bytes
  - **History Trend Charts** — Mini line chart (temperature changes) and mini bar chart (health history)
  - **Detailed Attributes Panel** — Scrollable list on the right, showing each SMART attribute's ID, name, current value, worst value, threshold, and raw value
    - Pre-failure attributes highlighted in red
    - Online attributes marked in blue
    - Threshold-exceeded attributes with yellow background
  - **Status Bar** — Bottom bar showing last refresh time, data validity, and SMART status summary
- **Overlay Mode** — Compact SMART health mini-window
  - Displays currently selected disk's health percentage, temperature, and read/write rates
  - Colored health status ring indicator
  - Transparency and always-on-top behavior consistent with the I/O overlay (25% default, 80% on hover)
- **Debug Trace System** — Thread-aware diagnostic logging framework (`SmartDebug.h`)
  - Ring buffer (8192 entries), thread-safe snapshot reads
  - 12 trace categories: Thread Lifecycle, Disk Discovery, IOCTL Calls, Data Refresh, Health Compute, Rate Compute, Attribute Parse, Window Events, Overlay Events, User Interaction, Errors & Exceptions
  - RAII scoped trace macro `SMART_TRACE_SCOPE`, auto-pairing begin/end with duration calculation
  - Compile-time toggle `SMART_DEBUG_ENABLED` to control tracing overhead
- **Debug Viewer Window** — Real-time trace visualization tool (`SmartDebugWindow`)
  - **Tab 1: Trace Log** — Scrollable trace entry list showing thread ID, category, timestamp, function name, message, duration
    - Category color coding (12 colors)
    - Auto-scroll / pause (Space key toggle)
    - Ctrl+L to toggle file logging
  - **Tab 2: Thread Stats** — Per-thread runtime statistics (entry count, error count, total duration, max duration, last activity)
  - **Tab 3: Flow Graph** — ASCII-art timeline showing execution flow of major operations
  - F5 refresh, Ctrl+C to clear all trace data
  - File logging output to `IO_SMART_Debug_YYYYMMDD_HHMMSS.log`
- **Data Collection Thread** — Independent low-priority refresh thread, default 120-second interval
  - Incremental rate calculation (LBA count difference between two snapshots / time interval)
  - Temperature history (`m_tempHistory` deque) for trend charts
  - Health percentage history (`m_healthHistory` deque) for health trends
- **Factory Pattern Disk Detection** — `createSmartReader()` auto-detects disk interface type
  - Prioritizes ATA IDENTIFY DEVICE, falls back to NVMe Identify Controller, finally SCSI

### 🔧 Build System

- `CMakeLists.txt` adds `setupapi` link library (required for `SetupDiGetClassDevs` APIs in disk enumeration)
- Project version bumped to `v1.7.0`
- Added 11 new source files (`SmartDataModel.cpp/.h`, `SmartReaderBase.cpp/.h`, `SmartReaderAta.cpp/.h`, `SmartReaderNvme.cpp/.h`, `SmartReaderScsi.cpp/.h`, `SmartMonitor.cpp/.h`, `SmartOverlayWindow.cpp/.h`, `SmartDebug.h`, `SmartDebugWindow.cpp/.h`)

---

### Added

- **IOPS & Queue Depth Monitoring** — New `IopsMonitor` sub-page module integrated into the advanced monitor (overlay window) framework
  - Queries `\PhysicalDisk(_Total)\Disk Reads/sec` and `\PhysicalDisk(_Total)\Disk Writes/sec` via PDH for real-time IOPS data
  - Queries `\PhysicalDisk(_Total)\Current Disk Queue Length` for disk queue depth
  - Overlay window now features an "IOPS & Queue" display section showing total IOPS (with R/W breakdown) and queue depth
  - Queue depth uses color coding: Green (<1), Yellow (1–5), Red (>5) for intuitive disk pressure visualization
- **Resource Optimization Mechanism** — IOPS monitoring thread is fully stopped and destroyed when advanced monitor mode (overlay) is inactive
  - `IopsMonitor::stop()` sequentially closes thread handle, removes PDH counters, and closes PDH query, ensuring zero background polling
  - `IopsMonitor::start()` dynamically initializes only when overlay is active, with 500ms sampling interval at lowest thread priority
- **Overlay UI Restructuring** — Redesigned into three distinct sections: Throughput, IOPS & Queue Depth, Cumulative Bytes

### Changed

- `OverlaySnapshot` struct gains `readIops`, `writeIops`, `totalIops`, `queueDepth`, `iopsValid` fields
- `OverlayWindow` dimensions expanded from 280×130 to 300×200 to accommodate new data sections
- `OverlayWindow::start()` launches IOPS monitor before creating the overlay window, with automatic rollback on failure
- `OverlayWindow::stop()` stops the IOPS monitor thread immediately after overlay destruction

---

## [v1.0.0] — 2026-07-20

### 🚀 Added

- **Core Monitoring Engine** — Real-time per-process I/O data collection based on `CreateToolhelp32Snapshot` + `GetProcessIoCounters`
- **Physical Disk Statistics** — Queries `\PhysicalDisk(_Total)\Disk Read/Write Bytes/sec` via PDH performance counters
- **Color Console UI** — Full TUI built on VT100/ANSI escape sequences, featuring box-drawing character borders, color highlighting, and differential rendering
- **Multi-Dimensional Sorting**
  - Sort by Total Rate (read + write) — key `T`
  - Sort by Read Rate — key `R`
  - Sort by Write Rate — key `W`
  - Sort by Session Cumulative I/O — key `S`
  - Sort by Process Lifetime Total I/O — key `P`
- **CSV Recording** — Export I/O data to structured CSV files in real time, with full process paths, rates, and cumulative statistics
  - Async write queue (max 200 entries), dedicated writer thread, zero blocking on main loop
  - Auto-creates `records/` output directory
  - Filename format: `IO_YYYYMMDD_HHMMSS.csv`
  - Supports auto-recording on launch (`-o` flag) and runtime toggle (`O` key)
- **Overlay Mini-Window** — 25% opaque always-on-top mini-window
  - Displays current total I/O rate, disk usage percentage, recording status, active process count
  - Becomes 80% opaque on mouse hover
  - Double-click or press `M` to return to full console
  - Independent Win32 window thread with `WS_EX_LAYERED` for transparency control
- **Dual-Line Process Display** — Each process shown across two lines: process name + full image path
- **Rate Trend Indicators** — Up/down arrows (↑/↓) marking I/O rate changes
- **Activity Color Coding** — Red (>10MB/s), Yellow (>1MB/s), Green (>0), Gray (idle)
- **PID Reuse Detection** — Automatically detects when a PID is recycled and resets historical counters
- **Command-Line Arguments** — Supports `-s`/`-r`/`-n`/`-o`/`-h` configuration parameters

### ⚡ Performance Optimizations

- Monitoring thread runs at `THREAD_PRIORITY_LOWEST` + `THREAD_MODE_BACKGROUND_BEGIN`
- Console UI uses row-level caching for differential updates, only redrawing changed rows
- Periodic cleanup of exited process historical data (every 60 sampling cycles)
- Executable size ~214KB, memory usage ~2-3MB

### 🧪 Testing

- Established complete Python automated test suite with **126 test cases** at 100% pass rate
- **Unit Tests (106 items)**
  - `test_monitor.py` (32 items): ProcessIOData struct behavior, sort logic, sampling intervals, PID reuse detection, thread safety
  - `test_formatting.py` (50 items): Rate formatting, byte formatting, time formatting, string truncation, CSV escaping, UTF-8 conversion
  - `test_recorder.py` (24 items): CSV file creation, queue operations, writer thread lifecycle, data integrity
- **Integration Tests (20 items)**
  - `test_integration.py`: Monitor + Recorder collaboration, full pipeline data collection/sorting/recording, error recovery
- **Test Infrastructure**
  - `pytest.ini` — Global pytest configuration with 7 custom markers
  - `conftest.py` — Global fixtures, environment diagnostics, automatic system info collection on failure
  - `test_runner.py` — Automated test runner supporting parallel execution and coverage collection
  - `run_tests.bat` — Windows one-click test script
- **Report System** — Auto-generates HTML (visual), JSON (structured), and Markdown (text summary) reports

### 🔧 Build System

- **CMake** — Supports MSVC and MinGW-w64 compilers
  - Auto-configures `pdh`, `psapi`, `gdi32` link libraries
  - Release mode enables `/O2 /GL` + `/LTCG` whole-program optimization
  - Sets `WIN32_LEAN_AND_MEAN` and `NOMINMAX` to reduce compilation footprint
- **Visual Studio** — Provides `.slnx` solution file

### Compatibility

- Operating System: Windows 10 version 1607 or later (VT100 terminal support required)
- Compiler: MSVC 2019+ / MinGW-w64 8.0+
- C++ Standard: C++17
- Python Test Environment: Python 3.7+ / pytest 7.4+

---

## Versioning

This project follows Semantic Versioning:

- **MAJOR**: Incompatible API changes or significant architectural overhauls
- **MINOR**: Backward-compatible feature additions
- **PATCH**: Backward-compatible bug fixes

---

## Legend

| Icon | Meaning |
|------|---------|
| 🚀 | New feature |
| ⚡ | Performance improvement |
| 🐛 | Bug fix |
| 📝 | Documentation update |
| 🧪 | Test-related |
| 🔧 | Build/toolchain changes |
| ⚠️ | Breaking change |
