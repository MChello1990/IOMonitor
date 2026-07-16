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
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <unordered_map>

#ifdef _MSC_VER
#pragma comment(lib, "pdh.lib")
#endif

struct ProcessIOData {
    DWORD pid = 0;
    std::wstring name;       // exe name only
    std::wstring path;       // full image path

    double readRate = 0.0;
    double writeRate = 0.0;

    uint64_t sessionReadBytes = 0;
    uint64_t sessionWriteBytes = 0;

    uint64_t processReadBytes = 0;
    uint64_t processWriteBytes = 0;

    bool active = false;

    uint64_t totalSessionIO() const { return sessionReadBytes + sessionWriteBytes; }
    double   totalRate()      const { return readRate + writeRate; }
};

struct SystemIOStats {
    double   physicalDiskReadRate = 0.0;
    double   physicalDiskWriteRate = 0.0;
    uint64_t monitoredReadRate = 0;
    uint64_t monitoredWriteRate = 0;
    int      activeProcessCount = 0;
    int      totalProcessCount = 0;
};

enum class SortMode {
    TOTAL_RATE,
    READ_RATE,
    WRITE_RATE,
    SESSION_TOTAL,
    SESSION_READ,
    SESSION_WRITE,
    PROCESS_TOTAL,
};

class DiskMonitor {
public:
    DiskMonitor();
    ~DiskMonitor();

    DiskMonitor(const DiskMonitor&) = delete;
    DiskMonitor& operator=(const DiskMonitor&) = delete;

    bool start(int sampleIntervalMs = 1000);
    void stop();
    bool isRunning() const { return m_running; }

    std::vector<ProcessIOData> getProcesses(SortMode sort, size_t maxCount) const;
    SystemIOStats             getSystemStats() const;

    void resetSessionTotals();

    int  getSampleInterval() const { return m_sampleIntervalMs.load(); }
    void setSampleInterval(int ms);

    // Notify that new data is available (for on-demand refresh)
    bool hasNewData() const { return m_hasNewData.load(); }
    void clearNewDataFlag() { m_hasNewData = false; }

private:
    void monitorLoop();
    void sample();

    struct ProcHistory {
        uint64_t readBytes = 0;
        uint64_t writeBytes = 0;
        std::chrono::steady_clock::time_point timestamp;
    };
    struct SessionAcc {
        uint64_t readBytes = 0;
        uint64_t writeBytes = 0;
    };

    mutable std::mutex m_mutex;
    std::vector<ProcessIOData> m_processes;
    SystemIOStats m_sysStats;
    std::unordered_map<DWORD, ProcHistory> m_history;
    std::unordered_map<DWORD, SessionAcc>  m_sessionAcc;

    PDH_HQUERY  m_pdhQuery = nullptr;
    PDH_HCOUNTER m_pdhRead = nullptr;
    PDH_HCOUNTER m_pdhWrite = nullptr;
    bool m_pdhOk = false;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hasNewData{false};
    std::atomic<int>  m_sampleIntervalMs{1000};

    HANDLE m_hThread = nullptr;
};
