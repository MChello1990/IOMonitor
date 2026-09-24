# IO Monitor Changelog

This document records all notable changes to the IO Monitor project.

---

## [v3.0.0] — 2026-09-24

This is a MAJOR release: the S.M.A.R.T. acquisition layer is replaced wholesale, from the in-house smartmontools-style readers to a CrystalDiskInfo port. It is an architectural swap, hence the major version. The `SmartDataModel` contract and all rendering and interaction code are unchanged.

### Added

- **`--smart-dump`** — A command-line switch that prints each disk's model, serial, firmware, interface, capacity, health verdict and full attribute table, followed by what the GUI adapter sees, for on-machine verification and troubleshooting.

### Refactoring

- **S.M.A.R.T. acquisition replaced by a CrystalDiskInfo port** — Removed the smartmontools-derived `SmartReaderBase` / `SmartReaderAta` / `SmartReaderNvme` / `SmartReaderScsi` (8 files). Disk enumeration, identification and S.M.A.R.T. reading are now handled by `CdiSmart*.h/cpp`, a port of CrystalDiskInfo's [`CAtaSmart`](https://github.com/hiyohiyo/CrystalDiskInfo) (MIT License). Scope is the mainstream Windows 10 / 11 paths: `IOCTL_ATA_PASS_THROUGH`, `DFP_RECEIVE_DRIVE_DATA`, `IOCTL_IDE_PASS_THROUGH`, SCSI ATA PASS-THROUGH (12) (SAT), the legacy `SMART_RCV_DRIVE_DATA` fallback, and NVMe via `IOCTL_STORAGE_QUERY_PROPERTY`. Vendor-private NVMe tunnels (Samsung, Intel RST/VROC, JMicron, ASMedia, Realtek RAID, MegaRAID, CSMI, AMD-RC2, Silicon Image) and INI-driven rules are out of scope.
- **Adapter layer and unit split** — The acquisition layer is split into 7 translation units (core / acquisition paths / attribute parsing / vendor detection / status verdict / NVMe interpreter / shared support), plus a new `SmartCdiAdapter` mapping `cdi::DRIVE_INFO` onto the GUI's `DiskIdentity` / `SmartAttribute` / `SmartDataSnapshot`. `SmartDataModel` and all rendering code are unchanged.

### Improvements

- **SMART Attributes moved to a full-width table** — The table has six columns, but the 340 px right column it used to live in could not hold a row (roughly 500 px of text at the mono font), so the Raw column was clipped away entirely and the hard-coded header never lined up with the rows. The table now occupies its own full-width band along the bottom of the window: each cell is drawn into its own rect (Value / Worst / Thresh right-aligned), the header aligns exactly with the data, over-long values are ellipsised instead of disappearing, and the list scrolls with the mouse wheel when it holds more rows than fit — with a "showing A-B of N" hint next to the title.
- **Attribute row colour now carries state, not category** — every pre-failure attribute used to be tinted yellow regardless of its value, so healthy drives showed yellow rows that conveyed nothing. Only attributes that have actually reached their threshold are highlighted now.
- **Vendor detection and health verdict** — CrystalDiskInfo's classification chain (37 predicates covering Samsung / Intel / Micron / SK hynix / Kioxia / WDC / SanDisk / Seagate / Kingston / SiliconMotion / Phison / Marvell / Realtek / YMTC and more) and its `CheckDiskStatus` verdict replace the previous single weighted score.
- **NVMe attribute naming** — The NVMe SMART log is mapped onto an ATA-style attribute list (IDs 0x01–0x1D) by CrystalDiskInfo's NVMeInterpreter, with names taken from its language file.

### Bug Fixes

- **SMART Attributes page showed three empty NVMe columns** — CrystalDiskInfo's NVMe interpreter fills `Id` and `RawValue` only (NVMe has no ATA-style normalised value / threshold page), so Value, Worst and Thresh stayed 0 and only Raw carried data. The adapter now synthesises those three columns from the NVMe log fields (100 = healthy, 0 = fault, with a threshold wherever the field has one — the same convention the pre-refactor NVMe reader used). Fault rows are highlighted by the existing colouring, and the Critical Warning bit field renders as readable text again (`temperature_high`, `read_only`, …).
- **NVMe health weights never applied** — `computeHealth` matched the NVMe weights by attribute *name*, but the port names them from CrystalDiskInfo's language table, where the media-error entry is `Media and Data Integrity Errors` rather than `Media Errors`; that weight could never fire. Now matched by NVMe attribute id (0x01 / 0x05 / 0x0D / 0x0E).
- **NVMe protocol query layout** — Fixed the query structure: CrystalDiskInfo declares its own 8-byte `{PropertyId, QueryType}` rather than the Windows SDK's `STORAGE_PROPERTY_QUERY` (which carries `AdditionalParameters[1]`, `sizeof == 12`). The SDK layout pushes the protocol data four bytes too late and the query is rejected with `ERROR_INVALID_PARAMETER` (87).
- **NVMe attribute out-of-bounds read** — `CdiAttributeName.cpp` read the six-byte `SMART_ATTRIBUTE::RawValue` with the eight-byte `B8toB64le(const BYTE*)`, picking up the following attribute's Id. Switched to the six-byte array overload, fixing controller-busy-time / power-on-hours / unsafe-shutdowns raw values that displayed as astronomical numbers.
- **Wide `wprintf`/`swprintf` format specifiers** — `%s` in a wide format string means a *narrow* string under a conforming C runtime (under MinGW with `-municode` it printed only the first byte), so every such site now uses `%ls`; MSVC accepts that spelling too.
- **Vendor detection and thresholds** — `Threshold05` / `ThresholdC5` / `ThresholdC6` / `ThresholdFF` are seeded with CrystalDiskInfo's INI defaults of 1/1/1/10, so CAUTION reporting is no longer silently disabled.

### Compatibility

- **Windows 11 NVMe fallback** — Some Windows 11 NVMe driver stacks (the Samsung driver, verified on this machine) answer `StorageAdapterProtocolSpecificProperty` (49) with `ERROR_INVALID_FUNCTION` while accepting the device-level `StorageDeviceProtocolSpecificProperty` (50). NVMe queries now try 49 and fall back to 50.
- **NVMe identity fallback** — The same driver stack serves the SMART health log but returns no Identify Controller payload; identity then comes from the storage descriptor, instead of dropping the drive (and its whole S.M.A.R.T. page).
- **MinGW builds** — Added the `FILE_DEVICE_SCSI` definition mingw-w64's `ntddscsi.h` lacks, and `_WIN32_WINNT=0x0A00` for MinGW builds.

### Build System

- **Source list swapped** — `CMakeLists.txt` and `IOMonitor.vcxproj` / `.filters` drop the 8 `SmartReader*.cpp/h` files in favour of the 10 acquisition-layer files (`CdiSmart`, `CdiSmartSupport`, `CdiSmartIo`, `CdiSmartFill`, `CdiSmartSsd`, `CdiSmartStatus`, `CdiSmartNvmeInterp`, `CdiAttributeName`, `CdiSmartAttributeTable`) plus `SmartCdiAdapter`.
- **SDK level aligned** — `_WIN32_WINNT=0x0A00 WINVER=0x0A00` is now defined globally, so the NVMe storage protocol query and `StorageAdapterProtocolSpecificProperty` are declared under both MSVC and MinGW.
- **Version bumped to `v3.0.0`** — `CMakeLists.txt`'s `project(... VERSION 3.0.0)`, the `main.cpp` help text, the `Display.cpp` title bar and the README screenshot are all updated in step.

### Documentation

- **README brought up to date** — the architecture diagram was redrawn for the new acquisition-layer files, `--smart-dump` was documented, and the compatibility notes (Administrator rights required to read S.M.A.R.T., and how the Windows 10 build relates to the NVMe direct path) were rewritten. A new "Acknowledgements" section credits CrystalDiskInfo (MIT) for the acquisition layer, NVMeInterpreter (MIT) for the NVMe SMART log parsing, and CrystalDiskInfo's `Language/English.lang` as the source of the attribute-name table.
- **`.gitignore`** — `/build`, `/build-*` and `/.idea/` are now ignored, keeping local build output and IDE settings out of commits.

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
