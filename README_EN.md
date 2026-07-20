# IO Monitor — Disk I/O Usage Monitor

## Overview

IO Monitor is a Windows-based disk I/O monitoring tool. It monitors the disk read/write activity of each process in real time, generates rate rankings and session cumulative statistics, and displays them through a colorful console user interface (CUI).

**Key Features:**
- Real-time per-process disk read/write rate monitoring
- Multi-dimensional ranking (read, write, total rate, session cumulative, process lifetime)
- Physical disk throughput statistics
- **CSV Recording** — Export I/O data to CSV files in real time
- **Overlay Mini-Window** — 25% opaque always-on-top mini-window for at-a-glance disk activity
- Extremely low CPU and memory footprint, ideal for long-running background monitoring
- Native console color interface, no graphical environment required

## Project Structure

```
IOMonitor/
├── src/
│   ├── main.cpp           # Entry point: argument parsing, component initialization
│   ├── Monitor.h/cpp      # Core: process I/O data collection & PDH physical disk stats
│   ├── Display.h/cpp      # UI: VT100 color console rendering & keyboard interaction
│   ├── Recorder.h/cpp     # Recording: CSV I/O data export (async write queue)
│   └── OverlayWindow.h/cpp # Overlay: Win32 always-on-top mini-window
│   └── reports/            # Test report output directory
├── CMakeLists.txt          # CMake build configuration

└── README.md
```

## Quick Start

### Build

**MSVC (Visual Studio):**

Open `IOMonitor.slnx` directly and build, or use CMake:

```bash
cmake -B build -G "Visual Studio 17 2022"
cmake --build build --config Release
```

**MinGW-w64:**

```bash
g++ -std=c++17 -O2 -DUNICODE -D_UNICODE \
    -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -o iomonitor.exe src/main.cpp src/Monitor.cpp src/Display.cpp \
    src/Recorder.cpp src/OverlayWindow.cpp \
    -lpdh -lpsapi -lgdi32 -municode -mconsole -static
```

### Run

```bash
# Run with default parameters (1000ms sampling, 500ms refresh, top 30 processes)
iomonitor.exe

# Custom parameters
iomonitor.exe -s 500 -r 300 -n 50

# Start CSV recording on launch
iomonitor.exe -o

# Show help
iomonitor.exe --help
```

> **Tip:** Run with administrator privileges for complete process coverage. Some system processes' I/O data may not be readable otherwise.

## Command-Line Arguments

| Argument | Short | Default | Range | Description |
|----------|-------|---------|-------|-------------|
| `--sample N` | `-s N` | 1000 | 200–10000 | I/O data sampling interval (ms) |
| `--refresh N` | `-r N` | 500 | 100–2000 | Display refresh interval (ms) |
| `--num N` | `-n N` | 30 | 5–100 | Number of processes displayed |
| `--record` | `-o` | — | — | Auto-start CSV recording on launch |
| `--help` | `-h` | — | — | Show help information |

## Interface

```
┌──── IO Monitor v1.0 ─────────────────────────────────────────────────┐
│  Up: 00:05:32  |  Procs: 12 active / 184 total  |  Disk: R 12.3M W 5.7M/s │
├───────────────────────────────────────────────────────────────────────┤
│  #   Process              PID    Read/sec       Write/sec      Session IO │
├───────────────────────────────────────────────────────────────────────┤
│  1   chrome.exe          12345     5.23 MB/s      1.12 MB/s      12.34 GB │
│     └ C:\Program Files\Google\Chrome\Application\chrome.exe              │
│  2   msedge.exe           5678     3.45 MB/s      0.89 MB/s       8.90 GB │
│     └ C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe       │
│  3   explorer.exe         9012     2.10 MB/s      0.52 MB/s       3.21 GB │
│     └ C:\Windows\explorer.exe                                             │
│  ...                                                                      │
├───────────────────────────────────────────────────────────────────────┤
│  Sort: Total Rate  |  Show: 30  |  Sample: 1000ms  |  REC  |  [Q]uit ... │
└───────────────────────────────────────────────────────────────────────┘
```

### Interface Elements

- **Title Bar** — Uptime, active/total process count, physical disk real-time throughput
- **Data Area** — Process ranking table (dual-line display: process name + full path), color-coded by activity level (Red >10MB/s, Yellow >1MB/s, Green >0)
- **Footer** — Current sort mode, display count, sampling/refresh interval, recording status indicator, shortcut hints

### Column Descriptions

| Column | Meaning |
|--------|---------|
| `#` | Current rank |
| `Process` | Process executable name (top line) + full image path (bottom line) |
| `PID` | Process ID |
| `Read/sec` | Current read rate (B/s ~ GB/s) |
| `Write/sec` | Current write rate (B/s ~ GB/s) |
| `Session IO` | Cumulative I/O since monitoring started (or last reset) |

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `R` | Sort by **Read Rate** (descending) |
| `W` | Sort by **Write Rate** (descending) |
| `T` | Sort by **Total Rate** (read + write, descending) |
| `S` | Sort by **Session Cumulative I/O** (descending) |
| `P` | Sort by **Process Lifetime I/O** (descending) |
| `C` | **Clear** all session cumulative statistics |
| `O` | **Toggle CSV recording** on/off |
| `M` | **Toggle overlay mini-window** (25% opaque always-on-top) |
| `1` | Set sampling interval to **200ms** (high-frequency) |
| `2` | Set sampling interval to **500ms** |
| `3` | Set sampling interval to **1000ms** (default) |
| `4` | Set sampling interval to **2000ms** |
| `5` | Set sampling interval to **5000ms** (low-frequency) |
| `+` / `=` | Increase displayed process count (+5) |
| `-` / `_` | Decrease displayed process count (-5) |
| `[` | Speed up display refresh (-100ms) |
| `]` | Slow down display refresh (+100ms) |
| `Q` / `Esc` | Quit |

## Sort Modes

| Mode | Key | Description | Use Case |
|------|-----|-------------|----------|
| **Total Rate** | `T` | Sort by current read + write rate | Find processes with the most active real-time disk I/O |
| **Read Rate** | `R` | Sort by current read rate | Troubleshoot processes with heavy reads |
| **Write Rate** | `W` | Sort by current write rate | Troubleshoot processes with heavy writes |
| **Session IO** | `S` | Sort by cumulative I/O during monitoring | Find processes with the highest long-term disk usage |
| **Process IO** | `P` | Sort by total I/O since process startup | Find processes with historically highest disk I/O |

## CSV Recording

Press `O` or use the `-o` launch argument to enable CSV recording. Recording files are saved in the `records/` subdirectory relative to the executable, with filenames in the format `IO_YYYYMMDD_HHMMSS.csv`.

### CSV Column Definitions

| Column | Description |
|--------|-------------|
| `Timestamp` | Sample timestamp (ISO 8601 format) |
| `PID` | Process ID |
| `ProcessName` | Process executable name |
| `ProcessPath` | Process full image path |
| `ReadRate(B/s)` | Current read rate (bytes/second) |
| `WriteRate(B/s)` | Current write rate (bytes/second) |
| `TotalRate(B/s)` | Total rate (read + write) |
| `SessionReadBytes` | Session cumulative read bytes |
| `SessionWriteBytes` | Session cumulative write bytes |
| `SessionTotalBytes` | Session cumulative total I/O bytes |
| `ProcessReadBytes` | Process lifetime read bytes |
| `ProcessWriteBytes` | Process lifetime write bytes |
| `Event` | Event type (`sample` / `start` / `stop`) |

### Recording Event Types

- `start` — Recording started
- `sample` — Periodic sample snapshot (written once per sampling interval)
- `stop` — Recording stopped

## Overlay Mini-Window

Press `M` to enter overlay mode. The overlay is a 25% opaque always-on-top mini-window displaying:

- Current total disk I/O rate (read + write)
- Disk usage percentage
- Recording status indicator
- Active process count

**Controls:**
- **Mouse hover** — Overlay becomes 80% opaque
- **Mouse leave** — Returns to 25% opacity
- **Double-click overlay** — Return to full console mode
- **Press `M` again** — Return to full console mode

## Technical Details

### Data Collection

1. **Process Enumeration** — Uses `CreateToolhelp32Snapshot` to capture a snapshot of all running processes
2. **I/O Counters** — Calls `GetProcessIoCounters` for each process to retrieve cumulative I/O counts (`IO_COUNTERS`)
3. **Rate Calculation** — Compares counter values between two consecutive samples, divides by elapsed time
4. **Physical Disk** — Queries `\PhysicalDisk(_Total)\Disk Read/Write Bytes/sec` performance counters via PDH (Performance Data Helper)
5. **Session Accumulation** — Maintains per-process I/O increment totals since monitoring started
6. **CSV Recording** — Async write queue with a dedicated writer thread, non-blocking for the main loop
7. **Overlay Window** — Independent Win32 window thread using `WS_EX_LAYERED` for transparency control

### Performance Design

- Monitoring thread runs at `THREAD_PRIORITY_LOWEST` + `THREAD_MODE_BACKGROUND_BEGIN`, never competing with foreground applications
- Each sample requires only ~3 API calls per process (`OpenProcess` → `GetProcessIoCounters` → `CloseHandle`), ~200 processes take <10ms
- Executable size ~214KB, memory usage ~2-3MB
- CSV recording uses async write queue (max 200 entries), dedicated writer thread, zero blocking
- PID reuse detection — automatically resets counters when a PID is recycled
- Console UI uses differential updates (row cache), only redraws changed rows

### Compatibility

- Operating System: Windows 10 version 1607 or later (VT100 terminal support required)
- Compiler: MSVC 2019+ or MinGW-w64 8.0+
- C++ Standard: C++17

## Automated Testing

The project includes local automated tests covering unit tests, integration tests, and report generation to ensure the stability and correctness of core functionality.

## FAQ

### Q: Why are some processes not showing?

Some system processes (e.g., protected anti-malware processes) may not be accessible via `PROCESS_QUERY_LIMITED_INFORMATION`. Run with **administrator privileges** for more complete process coverage.

### Q: Why is the physical disk rate showing 0?

If PDH counter initialization fails (e.g., in virtual machines or certain stripped-down Windows editions), the physical disk rate will display 0. Per-process I/O data will still display normally.

### Q: What's the difference between Session IO and Process Lifetime IO?

- **Session IO**: Only counts I/O during the monitor's runtime. Press `C` to reset and start fresh.
- **Process Lifetime IO**: Total I/O since the process started, reflecting its historical disk usage.

### Q: How do I reduce sampling overhead?

Increase the sampling interval (e.g., press `5` to switch to 5000ms mode) to further reduce CPU usage. For long-term background monitoring, a 2000–5000ms sampling interval is recommended.

### Q: Where are CSV recording files saved?

Recording files are saved in the `records/` subdirectory under the executable's directory. The directory is automatically created on launch.

### Q: How do I return to full console from overlay mode?

Double-click the overlay window or press `M` again to return to the full console mode.

### Q: Is remote monitoring supported?

The current version only supports local monitoring. Remote monitoring can be extended via pipes or network (WIP).
