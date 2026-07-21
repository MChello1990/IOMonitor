# IO Monitor Changelog

This document records all notable changes to the IO Monitor project.

---

## [v1.6.0] — 2026-07-20

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
