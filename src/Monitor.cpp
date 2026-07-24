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

#include "Monitor.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <thread>
#include <cmath>
#include <unordered_set>

DiskMonitor::DiskMonitor() {
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &m_pdhQuery);
    if (st == ERROR_SUCCESS) {
        if (PdhAddEnglishCounterW(m_pdhQuery,
                L"\\PhysicalDisk(_Total)\\Disk Read Bytes/sec", 0, &m_pdhReadRate) == ERROR_SUCCESS &&
            PdhAddEnglishCounterW(m_pdhQuery,
                L"\\PhysicalDisk(_Total)\\Disk Write Bytes/sec", 0, &m_pdhWriteRate) == ERROR_SUCCESS) {
            PdhCollectQueryData(m_pdhQuery);
            m_pdhOk = true;
        }
    }
    // Note: PDH does not provide a direct cumulative-bytes counter for
    // PhysicalDisk. Instead, we accumulate from the rate counters ourselves
    // in sample(), using the elapsed time between samples.
}

DiskMonitor::~DiskMonitor() {
    stop();
    if (m_pdhQuery) PdhCloseQuery(m_pdhQuery);
}

bool DiskMonitor::start(int sampleIntervalMs) {
    if (m_running) return true;
    m_sampleIntervalMs = sampleIntervalMs;
    m_running = true;
    m_hThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* self = static_cast<DiskMonitor*>(p);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        self->monitorLoop();
        return 0;
    }, this, 0, nullptr);
    return m_hThread != nullptr;
}

void DiskMonitor::stop() {
    m_running = false;
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 5000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
}

void DiskMonitor::monitorLoop() {
    while (m_running) {
        sample();
        auto total = std::chrono::milliseconds(m_sampleIntervalMs.load());
        auto step  = std::chrono::milliseconds(100);
        auto remaining = total;
        while (remaining.count() > 0 && m_running) {
            auto s = (std::min)(step, remaining);
            std::this_thread::sleep_for(s);
            remaining -= s;
        }
    }
}

void DiskMonitor::sample() {
    auto now = std::chrono::steady_clock::now();

    double pdhReadRate = 0.0, pdhWriteRate = 0.0;

    // Physical disk rate counters via PDH
    if (m_pdhOk) {
        PdhCollectQueryData(m_pdhQuery);
        PDH_FMT_COUNTERVALUE v;
        // Use PDH_FMT_DOUBLE for rate counters — more reliable than LARGE
        if (PdhGetFormattedCounterValue(m_pdhReadRate, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
            pdhReadRate = v.doubleValue;
        if (PdhGetFormattedCounterValue(m_pdhWriteRate, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS)
            pdhWriteRate = v.doubleValue;
    }

    // Accumulate cumulative bytes from rate × elapsed time
    // (PDH does not expose a direct cumulative-bytes counter for PhysicalDisk)
    {
        // NOTE: Using member variables instead of static locals to avoid
        // cross-instance sharing if multiple DiskMonitor instances exist.
        // The per-instance accumulation is safe since sample() is called
        // from a single dedicated thread per monitor instance.
        auto& lastSampleTime = m_lastSampleTime;
        auto& accReadBytes    = m_accumReadBytes;
        auto& accWriteBytes   = m_accumWriteBytes;

        double elapsed = std::chrono::duration<double>(now - lastSampleTime).count();
        if (elapsed > 0.0 && elapsed < 300.0) { // guard against huge gaps
            accReadBytes  += static_cast<uint64_t>(pdhReadRate * elapsed);
            accWriteBytes += static_cast<uint64_t>(pdhWriteRate * elapsed);
        }
        lastSampleTime = now;
    }

    HANDLE hSnap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnap == INVALID_HANDLE_VALUE) return;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (!Process32FirstW(hSnap, &pe)) { CloseHandle(hSnap); return; }

    std::vector<ProcessIOData> current;
    current.reserve(300);

    double monReadRate = 0.0, monWriteRate = 0.0;
    int activeCount = 0;

    do {
        DWORD pid = pe.th32ProcessID;
        if (pid <= 4) continue; // Idle (0) / System (4)

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!hProc) continue;

        IO_COUNTERS io{};
        BOOL ok = GetProcessIoCounters(hProc, &io);

        // Get full image path
        wchar_t imgPath[MAX_PATH] = {0};
        DWORD pathLen = MAX_PATH;
        if (ok) {
            QueryFullProcessImageNameW(hProc, 0, imgPath, &pathLen);
        }
        CloseHandle(hProc);
        if (!ok) continue;

        ProcessIOData d;
        d.pid  = pid;
        d.name = pe.szExeFile;
        d.path = imgPath;

        auto& hist = m_history[pid];
        auto& sess = m_sessionAcc[pid];

        double elapsed = 0.0;
        if (hist.timestamp.time_since_epoch().count() > 0)
            elapsed = std::chrono::duration<double>(now - hist.timestamp).count();

        bool reused = (io.ReadTransferCount  < hist.readBytes ||
                       io.WriteTransferCount < hist.writeBytes);

        if (reused) {
            // PID was reused — reset history and session for this PID
            m_sessionAcc[pid] = {};
            hist.readBytes  = io.ReadTransferCount;
            hist.writeBytes = io.WriteTransferCount;
            hist.timestamp  = now;
            d.sessionReadBytes  = 0;
            d.sessionWriteBytes = 0;
        } else if (elapsed > 0.0) {
            uint64_t dr = io.ReadTransferCount  - hist.readBytes;
            uint64_t dw = io.WriteTransferCount - hist.writeBytes;
            d.readRate  = static_cast<double>(dr) / elapsed;
            d.writeRate = static_cast<double>(dw) / elapsed;
            sess.readBytes  += dr;
            sess.writeBytes += dw;
            d.sessionReadBytes  = sess.readBytes;
            d.sessionWriteBytes = sess.writeBytes;

            hist.readBytes  = io.ReadTransferCount;
            hist.writeBytes = io.WriteTransferCount;
            hist.timestamp  = now;
        } else {
            d.sessionReadBytes  = sess.readBytes;
            d.sessionWriteBytes = sess.writeBytes;
            hist.readBytes  = io.ReadTransferCount;
            hist.writeBytes = io.WriteTransferCount;
            hist.timestamp  = now;
        }

        d.processReadBytes  = io.ReadTransferCount;
        d.processWriteBytes = io.WriteTransferCount;
        d.active = (d.readRate > 0.0 || d.writeRate > 0.0);

        if (d.active) activeCount++;

        monReadRate  += d.readRate;
        monWriteRate += d.writeRate;

        current.push_back(std::move(d));

    } while (Process32NextW(hSnap, &pe));
    CloseHandle(hSnap);

    // Clean up stale entries (processes no longer running)
    // Periodically every ~60 samples
    {
        static int cleanCounter = 0;
        if (++cleanCounter >= 60) {
            cleanCounter = 0;
            std::unordered_set<DWORD> alivePids;
            for (auto& p : current) alivePids.insert(p.pid);
            auto it = m_history.begin();
            while (it != m_history.end()) {
                if (alivePids.find(it->first) == alivePids.end())
                    it = m_history.erase(it);
                else
                    ++it;
            }
            auto sit = m_sessionAcc.begin();
            while (sit != m_sessionAcc.end()) {
                if (alivePids.find(sit->first) == alivePids.end())
                    sit = m_sessionAcc.erase(sit);
                else
                    ++sit;
            }
        }
    }

    // Update shared state — all writes under lock
    {
        int totalCount = static_cast<int>(current.size());

        std::lock_guard<std::mutex> lk(m_mutex);
        m_processes = std::move(current);
        m_sysStats.physicalDiskReadRate   = pdhReadRate;
        m_sysStats.physicalDiskWriteRate  = pdhWriteRate;
        m_sysStats.physicalDiskReadBytes  = m_accumReadBytes;
        m_sysStats.physicalDiskWriteBytes = m_accumWriteBytes;
        m_sysStats.monitoredReadRate  = static_cast<uint64_t>(monReadRate);
        m_sysStats.monitoredWriteRate = static_cast<uint64_t>(monWriteRate);
        m_sysStats.activeProcessCount  = activeCount;
        m_sysStats.totalProcessCount   = totalCount;
    }

    m_hasNewData = true;
}

std::vector<ProcessIOData> DiskMonitor::getProcesses(SortMode sort, size_t maxCount) const {
    std::lock_guard<std::mutex> lk(m_mutex);
    auto procs = m_processes;

    auto sortBy = [&](auto key) {
        std::sort(procs.begin(), procs.end(),
                  [&](const ProcessIOData& a, const ProcessIOData& b) { return key(a) > key(b); });
    };

    switch (sort) {
    case SortMode::TOTAL_RATE:    sortBy([](auto& x) { return x.totalRate(); });      break;
    case SortMode::READ_RATE:     sortBy([](auto& x) { return x.readRate; });         break;
    case SortMode::WRITE_RATE:    sortBy([](auto& x) { return x.writeRate; });        break;
    case SortMode::SESSION_TOTAL: sortBy([](auto& x) { return x.totalSessionIO(); }); break;
    case SortMode::SESSION_READ:  sortBy([](auto& x) { return x.sessionReadBytes; });  break;
    case SortMode::SESSION_WRITE: sortBy([](auto& x) { return x.sessionWriteBytes; }); break;
    case SortMode::PROCESS_TOTAL:
        sortBy([](auto& x) { return x.processReadBytes + x.processWriteBytes; });
        break;
    }

    if (procs.size() > maxCount) procs.resize(maxCount);
    return procs;
}

SystemIOStats DiskMonitor::getSystemStats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_sysStats;
}

void DiskMonitor::resetSessionTotals() {
    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto& [_, s] : m_sessionAcc) { s.readBytes = 0; s.writeBytes = 0; }
    // Also reset current process display
    for (auto& p : m_processes) { p.sessionReadBytes = 0; p.sessionWriteBytes = 0; }
}

void DiskMonitor::setSampleInterval(int ms) {
    if (ms >= 200 && ms <= 10000) m_sampleIntervalMs = ms;
}
