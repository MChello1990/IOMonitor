/*
 * Copyright (C) 2026 Nick Edson
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * GPL-3.0-or-later any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <pdh.h>
#include <string>
#include <atomic>
#include <mutex>
#include <chrono>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

// ── IOPS & Queue Depth statistics snapshot ──────────────────────────
struct IopsSnapshot {
    double readIops     = 0.0;   // reads/sec
    double writeIops    = 0.0;   // writes/sec
    double totalIops    = 0.0;   // total I/O operations/sec
    double queueDepth   = 0.0;   // current avg disk queue length
    bool   valid        = false; // whether data is available
};

// ── IopsMonitor: dedicated thread for IOPS & queue depth monitoring ─
// This class is designed to be completely started/stopped on demand.
// When stop() is called, the monitoring thread is fully destroyed,
// releasing all resources (PDH handles, thread handle). No background
// polling occurs while stopped.
class IopsMonitor {
public:
    IopsMonitor();
    ~IopsMonitor();

    IopsMonitor(const IopsMonitor&) = delete;
    IopsMonitor& operator=(const IopsMonitor&) = delete;

    // Start the monitoring thread. Creates PDH query and counters.
    // samplingIntervalMs: how often to poll PDH (default 500ms)
    bool start(int samplingIntervalMs = 500);

    // Stop the monitoring thread and destroy all resources.
    // After calling stop(), no background activity remains.
    void stop();

    bool isRunning() const { return m_running.load(); }

    // Get the latest IOPS/queue depth snapshot (thread-safe)
    IopsSnapshot getSnapshot() const;

private:
    void monitorLoop();

    // PDH handles — created on start(), destroyed on stop()
    PDH_HQUERY   m_pdhQuery      = nullptr;
    PDH_HCOUNTER m_pdhReadIops   = nullptr;
    PDH_HCOUNTER m_pdhWriteIops  = nullptr;
    PDH_HCOUNTER m_pdhQueueDepth = nullptr;
    bool         m_pdhOk         = false;

    // Monitoring thread
    std::atomic<bool> m_running{false};
    std::atomic<int>  m_sampleIntervalMs{500};
    HANDLE            m_hThread = nullptr;

    // Latest snapshot (protected by mutex)
    mutable std::mutex m_mutex;
    IopsSnapshot       m_snapshot;
};
