#include "IopsMonitor.h"
#include <thread>
#include <algorithm>

// ── Constructor ─────────────────────────────────────────────────────
IopsMonitor::IopsMonitor() {}

// ── Destructor: ensure everything is stopped ────────────────────────
IopsMonitor::~IopsMonitor() {
    stop();
}

// ── Start monitoring thread ─────────────────────────────────────────
bool IopsMonitor::start(int samplingIntervalMs) {
    if (m_running.load()) return true; // already running

    m_sampleIntervalMs = samplingIntervalMs;

    // Open PDH query
    PDH_STATUS st = PdhOpenQueryW(nullptr, 0, &m_pdhQuery);
    if (st != ERROR_SUCCESS) return false;

    // Add counters for IOPS and queue depth
    bool countersOk = true;

    if (PdhAddEnglishCounterW(m_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Reads/sec", 0, &m_pdhReadIops) != ERROR_SUCCESS) {
        countersOk = false;
    }
    if (PdhAddEnglishCounterW(m_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Disk Writes/sec", 0, &m_pdhWriteIops) != ERROR_SUCCESS) {
        countersOk = false;
    }
    if (PdhAddEnglishCounterW(m_pdhQuery,
            L"\\PhysicalDisk(_Total)\\Current Disk Queue Length", 0, &m_pdhQueueDepth) != ERROR_SUCCESS) {
        countersOk = false;
    }

    if (!countersOk) {
        // At least one counter failed — but we may still have partial data
        // Mark PDH as not fully OK, but still allow partial operation
    }

    // Collect initial data to establish baseline
    PdhCollectQueryData(m_pdhQuery);
    m_pdhOk = true;

    // Start background thread
    m_running = true;
    m_hThread = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        auto* self = static_cast<IopsMonitor*>(p);
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        self->monitorLoop();
        return 0;
    }, this, 0, nullptr);

    return m_hThread != nullptr;
}

// ── Stop monitoring thread and destroy all resources ────────────────
void IopsMonitor::stop() {
    m_running = false;

    // Wait for thread to exit
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 5000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }

    // Close PDH handles — release all OS resources
    if (m_pdhQueueDepth) {
        PdhRemoveCounter(m_pdhQueueDepth);
        m_pdhQueueDepth = nullptr;
    }
    if (m_pdhWriteIops) {
        PdhRemoveCounter(m_pdhWriteIops);
        m_pdhWriteIops = nullptr;
    }
    if (m_pdhReadIops) {
        PdhRemoveCounter(m_pdhReadIops);
        m_pdhReadIops = nullptr;
    }
    if (m_pdhQuery) {
        PdhCloseQuery(m_pdhQuery);
        m_pdhQuery = nullptr;
    }

    m_pdhOk = false;
}

// ── Background monitoring loop ──────────────────────────────────────
void IopsMonitor::monitorLoop() {
    while (m_running.load()) {
        // Collect PDH data
        if (m_pdhOk && m_pdhQuery) {
            PDH_STATUS st = PdhCollectQueryData(m_pdhQuery);
            if (st == ERROR_SUCCESS) {
                IopsSnapshot snap;
                snap.valid = true;

                PDH_FMT_COUNTERVALUE v;

                if (m_pdhReadIops &&
                    PdhGetFormattedCounterValue(m_pdhReadIops, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                    snap.readIops = v.doubleValue;
                }

                if (m_pdhWriteIops &&
                    PdhGetFormattedCounterValue(m_pdhWriteIops, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                    snap.writeIops = v.doubleValue;
                }

                if (m_pdhQueueDepth &&
                    PdhGetFormattedCounterValue(m_pdhQueueDepth, PDH_FMT_DOUBLE, nullptr, &v) == ERROR_SUCCESS) {
                    snap.queueDepth = v.doubleValue;
                }

                snap.totalIops = snap.readIops + snap.writeIops;

                // Update shared snapshot
                {
                    std::lock_guard<std::mutex> lk(m_mutex);
                    m_snapshot = snap;
                }
            }
        }

        // Sleep for the sampling interval, with sub-interval polling for fast shutdown
        auto total = std::chrono::milliseconds(m_sampleIntervalMs.load());
        auto step  = std::chrono::milliseconds(100);
        auto remaining = total;
        while (remaining.count() > 0 && m_running.load()) {
            auto s = (std::min)(step, remaining);
            std::this_thread::sleep_for(s);
            remaining -= s;
        }
    }
}

// ── Get latest snapshot (thread-safe) ───────────────────────────────
IopsSnapshot IopsMonitor::getSnapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_snapshot;
}
