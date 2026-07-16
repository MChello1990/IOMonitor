#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Monitor.h"
#include <string>
#include <atomic>
#include <chrono>

class ConsoleDisplay {
public:
    ConsoleDisplay();
    ~ConsoleDisplay();

    void run(DiskMonitor& monitor);

    void setRefreshMs(int ms)  { if (ms >= 100 && ms <= 2000) m_refreshMs = ms; }
    void setMaxDisplay(int n)  { if (n >= 5 && n <= 100) m_maxDisplay = n; }

private:
    void setupConsole();
    void restoreConsole();

    void renderHeader(const SystemIOStats& stats);
    void renderProcessRows(const std::vector<ProcessIOData>& procs, int maxDataRows);
    void renderFooter(int sampleMs);
    bool handleInput(DiskMonitor& monitor);

    std::wstring fmtRate(double bytesPerSec) const;
    std::wstring fmtBytes(uint64_t bytes) const;
    std::wstring fmtTime(int64_t seconds) const;
    std::wstring truncate(const std::wstring& s, size_t maxLen) const;

    void write(const std::wstring& text);

    SortMode m_sortMode = SortMode::TOTAL_RATE;
    int m_refreshMs  = 500;
    int m_maxDisplay = 30;
    std::atomic<bool> m_running{false};
    std::chrono::steady_clock::time_point m_startTime;

    HANDLE m_hOut;
    HANDLE m_hIn;
    DWORD  m_oldOutMode = 0;
    DWORD  m_oldInMode  = 0;
    UINT   m_oldOutputCP = 0;

    int m_consoleW = 120;
    int m_consoleH = 40;
    int m_headerLines = 6;   // rows used by header block (1-indexed)
    int m_footerLine  = 0;

    // Track previous row contents for differential (on-demand) update
    struct RowCache {
        std::wstring content;
        bool valid = false;
    };
    std::vector<RowCache> m_rowCache;

    // Previous sample data for rate change indicators
    std::vector<ProcessIOData> m_prevProcs;
};
