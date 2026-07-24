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

#include "Display.h"
#include "SmartMonitor.h"
#include <conio.h>
#include <cstdio>
#include <algorithm>
#include <thread>
#include <chrono>
#include <cmath>

// ── VT100 / ANSI escape sequences ───────────────────────────────────
static constexpr const wchar_t* CU_HOME   = L"\x1b[H";
static constexpr const wchar_t* CLS       = L"\x1b[2J";
static constexpr const wchar_t* CLR_BELOW = L"\x1b[J";
static constexpr const wchar_t* HIDE_CUR  = L"\x1b[?25l";
static constexpr const wchar_t* SHOW_CUR  = L"\x1b[?25h";

static std::wstring cuPos(int row, int col) {
    wchar_t buf[24];
    swprintf(buf, 24, L"\x1b[%d;%dH", row, col);
    return buf;
}

// ── color constants ─────────────────────────────────────────────────
namespace C {
    constexpr const wchar_t* RST     = L"\x1b[0m";
    constexpr const wchar_t* BOLD    = L"\x1b[1m";
    constexpr const wchar_t* DIM     = L"\x1b[2m";
    constexpr const wchar_t* RED     = L"\x1b[91m";
    constexpr const wchar_t* GREEN   = L"\x1b[92m";
    constexpr const wchar_t* YELLOW  = L"\x1b[93m";
    constexpr const wchar_t* CYAN    = L"\x1b[96m";
    constexpr const wchar_t* WHITE   = L"\x1b[97m";
    constexpr const wchar_t* GRAY    = L"\x1b[90m";
    constexpr const wchar_t* BLUE    = L"\x1b[94m";
}

// ── box-drawing glyphs ──────────────────────────────────────────────
static constexpr const wchar_t* BOX_H  = L"\u2500"; // ─
static constexpr const wchar_t* BOX_V  = L"\u2502"; // │
static constexpr const wchar_t* BOX_TL = L"\u250c"; // ┌
static constexpr const wchar_t* BOX_TR = L"\u2510"; // ┐
static constexpr const wchar_t* BOX_BL = L"\u2514"; // └
static constexpr const wchar_t* BOX_BR = L"\u2518"; // ┘
static constexpr const wchar_t* BOX_LR = L"\u251c"; // ├
static constexpr const wchar_t* BOX_RL = L"\u2524"; // ┤

static std::wstring repeat(const wchar_t* s, int n) {
    std::wstring r; r.reserve(static_cast<size_t>(n) * 3);
    for (int i = 0; i < n; ++i) r += s;
    return r;
}

// ── constructor / destructor ────────────────────────────────────────
ConsoleDisplay::ConsoleDisplay() {
    m_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    m_hIn  = GetStdHandle(STD_INPUT_HANDLE);
}

ConsoleDisplay::~ConsoleDisplay() { restoreConsole(); }

void ConsoleDisplay::setupConsole() {
    GetConsoleMode(m_hOut, &m_oldOutMode);
    GetConsoleMode(m_hIn,  &m_oldInMode);
    m_oldOutputCP = GetConsoleOutputCP();

    DWORD mode = m_oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                 DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(m_hOut, mode);
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    SetConsoleTitleW(L"IO Monitor - Disk I/O Usage Monitor");

    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    GetConsoleScreenBufferInfo(m_hOut, &csbi);
    m_consoleW = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    m_consoleH = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (m_consoleW < 100) m_consoleW = 100;
    if (m_consoleH < 20)  m_consoleH = 20;

    m_footerLine = m_consoleH;
    int dataRows = m_consoleH - m_headerLines - 1; // -1 for footer
    m_rowCache.resize(static_cast<size_t>(std::max(dataRows, 0)));

    write(std::wstring(CLS) + HIDE_CUR);
}

void ConsoleDisplay::restoreConsole() {
    write(std::wstring(SHOW_CUR) + C::RST + L"\n");
    if (m_oldOutMode) SetConsoleMode(m_hOut, m_oldOutMode);
    if (m_oldInMode)  SetConsoleMode(m_hIn,  m_oldInMode);
    if (m_oldOutputCP) SetConsoleOutputCP(m_oldOutputCP);
}

void ConsoleDisplay::write(const std::wstring& text) {
    DWORD n;
    WriteConsoleW(m_hOut, text.c_str(), static_cast<DWORD>(text.size()), &n, nullptr);
}

// ── formatting helpers ──────────────────────────────────────────────
std::wstring ConsoleDisplay::fmtRate(double bps) const {
    if (bps < 0.5) return L"    0.00  B/s";
    const wchar_t* u[] = { L" B/s", L"KB/s", L"MB/s", L"GB/s" };
    int i = 0; double v = bps;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    wchar_t b[32];
    swprintf(b, 32, L"%6.2f %s", v, u[i]);
    return b;
}

std::wstring ConsoleDisplay::fmtBytes(uint64_t bytes) const {
    if (bytes == 0) return L"     0  B";
    const wchar_t* u[] = { L" B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0; double v = static_cast<double>(bytes);
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    wchar_t b[32];
    if (v >= 100.0)      swprintf(b, 32, L"%5.0f %s", v, u[i]);
    else if (v >= 10.0)  swprintf(b, 32, L"%5.1f %s", v, u[i]);
    else                 swprintf(b, 32, L"%5.2f %s", v, u[i]);
    return b;
}

std::wstring ConsoleDisplay::fmtTime(int64_t sec) const {
    int h = static_cast<int>(sec / 3600);
    int m = static_cast<int>((sec % 3600) / 60);
    int s = static_cast<int>(sec % 60);
    wchar_t b[16]; swprintf(b, 16, L"%02d:%02d:%02d", h, m, s);
    return b;
}

std::wstring ConsoleDisplay::truncate(const std::wstring& s, size_t maxLen) const {
    if (s.size() <= maxLen) return s;
    if (maxLen <= 3) return s.substr(0, maxLen);
    return s.substr(0, maxLen - 2) + L"..";
}

// ── main loop (on-demand refresh, no pagination) ────────────────────
void ConsoleDisplay::run(DiskMonitor& monitor, Recorder& recorder) {
    setupConsole();
    m_running   = true;
    m_startTime = std::chrono::steady_clock::now();

    // Initial full render
    {
        auto procs = monitor.getProcesses(m_sortMode, static_cast<size_t>(m_maxDisplay));
        auto stats = monitor.getSystemStats();
        renderHeader(stats);
        int dataRows = m_consoleH - m_headerLines - 1;
        renderProcessRows(procs, dataRows);
        renderFooter(monitor.getSampleInterval(), recorder.isRecording());
    }

    while (m_running) {
        // Check if overlay mode was requested
        if (m_overlayModeRequested) {
            m_overlayModeRequested = false;
            if (enterOverlayMode(monitor, recorder)) {
                // Overlay window returned — restore full console
                // Clear row cache to force full redraw
                for (auto& rc : m_rowCache) rc.valid = false;
                write(CLS);
                // Continue the main loop: will render fresh on next iteration
                continue;
            }
        }

        // Check if SMART monitor page was requested
        if (m_smartMonitorRequested) {
            m_smartMonitorRequested = false;
            if (enterSmartMonitorMode(monitor, recorder)) {
                // SMART monitor closed — restore full console
                for (auto& rc : m_rowCache) rc.valid = false;
                write(CLS);
                continue;
            }
        }

        bool needRender = false;

        // Wait for new data or user input
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_refreshMs);
        while (std::chrono::steady_clock::now() < deadline && m_running) {
            if (_kbhit()) {
                if (!handleInput(monitor, recorder)) {
                    restoreConsole();
                    return;
                }
                needRender = true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }

        if (!m_running) break;

        // On-demand: only render when new sample data available or input changed state
        if (monitor.hasNewData() || needRender) {
            monitor.clearNewDataFlag();
            auto procs = monitor.getProcesses(m_sortMode, static_cast<size_t>(m_maxDisplay));
            auto stats = monitor.getSystemStats();

            // Enqueue sample for CSV recording if active
            if (recorder.isRecording()) {
                recorder.enqueueSample(procs);
            }

            // Handle console resize
            CONSOLE_SCREEN_BUFFER_INFO csbi{};
            GetConsoleScreenBufferInfo(m_hOut, &csbi);
            int newW = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            int newH = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
            if (newW < 100) newW = 100;
            if (newH < 20)  newH = 20;

            if (newW != m_consoleW || newH != m_consoleH) {
                m_consoleW = newW;
                m_consoleH = newH;
                m_footerLine = m_consoleH;
                int dataRows = m_consoleH - m_headerLines - 1;
                m_rowCache.resize(static_cast<size_t>(std::max(dataRows, 0)));
                for (auto& rc : m_rowCache) rc.valid = false;
                write(CLS);
            }

            renderHeader(stats);
            int dataRows = m_consoleH - m_headerLines - 1;
            renderProcessRows(procs, dataRows);
            renderFooter(monitor.getSampleInterval(), recorder.isRecording());

            m_prevProcs = procs;
        }
    }
    restoreConsole();
}

// ── keyboard input ──────────────────────────────────────────────────
bool ConsoleDisplay::handleInput(DiskMonitor& monitor, Recorder& recorder) {
    int ch = _getch();
    if (ch == 0 || ch == 224) { _getch(); return true; }

    switch (ch) {
    case 'q': case 'Q': case 27: m_running = false; return false;
    case 'r': case 'R': m_sortMode = SortMode::READ_RATE;     break;
    case 'w': case 'W': m_sortMode = SortMode::WRITE_RATE;    break;
    case 't': case 'T': m_sortMode = SortMode::TOTAL_RATE;    break;
    case 's': case 'S': m_sortMode = SortMode::SESSION_TOTAL; break;
    case 'p': case 'P': m_sortMode = SortMode::PROCESS_TOTAL; break;
    case 'c': case 'C': monitor.resetSessionTotals();         break;
    case 'o': case 'O':
        // Toggle CSV recording
        if (recorder.isRecording()) {
            recorder.stop();
        } else {
            if (!recorder.start()) {
                // Could not start — silently ignore (footer will still show OFF)
            }
        }
        break;
    case 'm': case 'M': {
        // Enter overlay mini-window mode
        // Need to return false to let run() handle the mode switch
        m_overlayModeRequested = true;
        return true;
    }
    case 'd': case 'D': {
        // Open SMART disk health monitor sub-page
        m_smartMonitorRequested = true;
        return true;
    }
    case '+': case '=': if (m_maxDisplay < 100) m_maxDisplay += 5;   break;
    case '-': case '_': if (m_maxDisplay > 5)   m_maxDisplay -= 5;   break;
    case '[': if (m_refreshMs > 100)  m_refreshMs -= 100; break;
    case ']': if (m_refreshMs < 2000) m_refreshMs += 100; break;
    case '1': monitor.setSampleInterval(200);  break;
    case '2': monitor.setSampleInterval(500);  break;
    case '3': monitor.setSampleInterval(1000); break;
    case '4': monitor.setSampleInterval(2000); break;
    case '5': monitor.setSampleInterval(5000); break;
    default: break;
    }
    return true;
}

// ── header rendering ────────────────────────────────────────────────
void ConsoleDisplay::renderHeader(const SystemIOStats& stats) {
    auto now    = std::chrono::steady_clock::now();
    auto uptime = std::chrono::duration_cast<std::chrono::seconds>(now - m_startTime).count();

    int innerW = m_consoleW - 2;

    // Row 1: top border
    write(cuPos(1, 1) + std::wstring(C::WHITE) + BOX_TL + repeat(BOX_H, innerW) + BOX_TR + C::RST);

    // Row 2: title bar
    {
        wchar_t tmp[256];
        swprintf(tmp, 256,
                 L"  IO Monitor v2.0   |   Up: %s   |   Active: %d / %d   |   "
                 L"Disk: R %s  W %s",
                 fmtTime(uptime).c_str(),
                 stats.activeProcessCount, stats.totalProcessCount,
                 fmtRate(stats.physicalDiskReadRate).c_str(),
                 fmtRate(stats.physicalDiskWriteRate).c_str());
        std::wstring t = tmp;
        if (static_cast<int>(t.size()) > innerW - 2)
            t = t.substr(0, static_cast<size_t>(innerW) - 2);
        write(cuPos(2, 1) + std::wstring(C::BOLD) + C::WHITE + BOX_V + C::RST + L" "
              + C::BOLD + C::CYAN + t + C::RST
              + std::wstring(static_cast<size_t>(std::max(innerW - static_cast<int>(t.size()) - 1, 0)), L' ')
              + C::WHITE + BOX_V + C::RST);
    }

    // Row 3: separator
    write(cuPos(3, 1) + std::wstring(C::WHITE) + BOX_LR + repeat(BOX_H, innerW) + BOX_RL + C::RST);

    // Row 4: column headers
    {
        const int RANK_W = 4;
        const int PID_W  = 8;
        const int RATE_W = 14;
        const int SESS_W = 14;
        int nameW = innerW - 2 - RANK_W - PID_W - RATE_W * 2 - SESS_W - 6;
        if (nameW < 16) nameW = 16;

        wchar_t tmp[512];
        swprintf(tmp, 512, L" %-*s%-*s%-*s %-*s %-*s %-*s ",
                 RANK_W, L"#",
                 nameW, L"Process Name / Path",
                 PID_W,  L"PID",
                 RATE_W, L"Read/sec",
                 RATE_W, L"Write/sec",
                 SESS_W, L"Session IO");
        write(cuPos(4, 1) + std::wstring(C::YELLOW) + C::BOLD + BOX_V + C::RST + L" " + tmp
              + std::wstring(static_cast<size_t>(std::max(innerW - static_cast<int>(wcslen(tmp)) - 1, 0)), L' ')
              + C::YELLOW + C::BOLD + BOX_V + C::RST);
    }

    // Row 5: separator
    write(cuPos(5, 1) + std::wstring(C::WHITE) + BOX_LR + repeat(BOX_H, innerW) + BOX_RL + C::RST);

    m_headerLines = 6;
}

// ── process rows (dual-line per process: name + path) ───────────────
void ConsoleDisplay::renderProcessRows(const std::vector<ProcessIOData>& procs, int maxDataRows) {
    int innerW = m_consoleW - 2;
    const int RANK_W = 4;
    const int PID_W  = 8;
    const int RATE_W = 14;
    const int SESS_W = 14;
    int nameW = innerW - 2 - RANK_W - PID_W - RATE_W * 2 - SESS_W - 6;
    if (nameW < 16) nameW = 16;

    // Each process uses 2 rows (name line + path line)
    // So effective display count = floor(maxDataRows / 2)
    int maxProcs = maxDataRows / 2;
    int displayCount = std::min(static_cast<int>(procs.size()), maxProcs);
    if (displayCount < 0) displayCount = 0;

    // Color based on activity level
    auto activityColor = [&](const ProcessIOData& p) -> const wchar_t* {
        double rate = p.totalRate();
        if (rate > 10.0 * 1024 * 1024)  return C::RED;     // > 10 MB/s
        if (rate > 1.0 * 1024 * 1024)   return C::YELLOW;  // > 1 MB/s
        if (rate > 0.0)                 return C::GREEN;
        return C::GRAY;
    };

    // Compare with previous for rate-change indicators
    auto rateTrend = [&](const ProcessIOData& cur, double prevRate) -> const wchar_t* {
        if (prevRate <= 0.0) return L" ";
        double diff = cur.totalRate() - prevRate;
        if (diff > 0.1 * 1024 * 1024) return L"\u2191"; // ↑
        if (diff < -0.1 * 1024 * 1024) return L"\u2193"; // ↓
        return L" ";
    };

    for (int i = 0; i < maxProcs; ++i) {
        int row1 = m_headerLines + i * 2;      // name row
        int row2 = row1 + 1;                    // path row
        int cacheIdx1 = i * 2;
        int cacheIdx2 = cacheIdx1 + 1;

        std::wstring line1, line2;

        if (i < displayCount) {
            auto& p = procs[i];
            const wchar_t* color = activityColor(p);

            // Find previous rate for trend indicator
            double prevRate = 0.0;
            for (auto& prev : m_prevProcs) {
                if (prev.pid == p.pid) { prevRate = prev.totalRate(); break; }
            }
            const wchar_t* trend = rateTrend(p, prevRate);

            // ── Line 1: name + pid + rates + session IO ──
            {
                wchar_t tmp[1024];
                swprintf(tmp, 1024, L" %*zu %-*s %*lu  %-*s %-*s %-*s ",
                         RANK_W, static_cast<size_t>(i + 1),
                         nameW, truncate(p.name, static_cast<size_t>(nameW)).c_str(),
                         PID_W,  p.pid,
                         RATE_W, fmtRate(p.readRate).c_str(),
                         RATE_W, fmtRate(p.writeRate).c_str(),
                         SESS_W, fmtBytes(p.totalSessionIO()).c_str());

                line1 = cuPos(row1, 1) + std::wstring(C::WHITE) + BOX_V + C::RST + L" " + std::wstring(color) + tmp + C::RST;
                int pad = innerW - static_cast<int>(wcslen(tmp)) - 1;
                if (pad > 0) line1 += std::wstring(pad, L' ');
                line1 += std::wstring(C::WHITE) + BOX_V + C::RST;
            }

            // ── Line 2: path ──
            {
                std::wstring pathDisplay;
                if (!p.path.empty()) {
                    pathDisplay = std::wstring(L"  \u2514 ") + C::DIM + p.path + C::RST; // └ path
                } else {
                    pathDisplay = std::wstring(L"  \u2514 ") + C::DIM + L"(access denied)" + C::RST;
                }
                // Truncate to fit
                if (static_cast<int>(pathDisplay.size()) > innerW - 2)
                    pathDisplay = std::wstring(L"  \u2514 ") + C::DIM
                                  + truncate(p.path.empty() ? L"(access denied)" : p.path,
                                             static_cast<size_t>(innerW - 5))
                                  + C::RST;

                line2 = cuPos(row2, 1) + std::wstring(C::WHITE) + BOX_V + C::RST + L" " + pathDisplay;
                int pad = innerW - static_cast<int>(pathDisplay.size()) - 1;
                if (pad > 0) line2 += std::wstring(pad, L' ');
                line2 += std::wstring(C::WHITE) + BOX_V + C::RST;
            }
        } else {
            // Empty row
            line1 = cuPos(row1, 1) + std::wstring(C::WHITE) + BOX_V + C::RST + L" "
                    + std::wstring(static_cast<size_t>(innerW) - 2, L' ') + L" "
                    + std::wstring(C::WHITE) + BOX_V + C::RST;
            line2 = cuPos(row2, 1) + std::wstring(C::WHITE) + BOX_V + C::RST + L" "
                    + std::wstring(static_cast<size_t>(innerW) - 2, L' ') + L" "
                    + std::wstring(C::WHITE) + BOX_V + C::RST;
        }

        // Differential update: only write if content changed
        if (cacheIdx1 < static_cast<int>(m_rowCache.size())) {
            if (!m_rowCache[cacheIdx1].valid || m_rowCache[cacheIdx1].content != line1) {
                write(line1);
                m_rowCache[cacheIdx1].content = line1;
                m_rowCache[cacheIdx1].valid = true;
            }
        } else {
            write(line1);
        }

        if (cacheIdx2 < static_cast<int>(m_rowCache.size())) {
            if (!m_rowCache[cacheIdx2].valid || m_rowCache[cacheIdx2].content != line2) {
                write(line2);
                m_rowCache[cacheIdx2].content = line2;
                m_rowCache[cacheIdx2].valid = true;
            }
        } else {
            write(line2);
        }
    }
}

// ── footer ──────────────────────────────────────────────────────────
void ConsoleDisplay::renderFooter(int sampleMs, bool isRecording) {
    int innerW = m_consoleW - 2;

    // Separator above footer
    write(cuPos(m_consoleH - 1, 1) + std::wstring(C::WHITE) + BOX_LR + repeat(BOX_H, innerW) + BOX_RL + C::RST);

    // Footer text
    const wchar_t* sortNames[] = {
        L"Total Rate", L"Read Rate", L"Write Rate",
        L"Session IO", L"Session Rd", L"Session Wr", L"Process IO"
    };

    // Recording status indicator
    std::wstring recStatus;
    if (isRecording) {
        recStatus = std::wstring(C::RED) + L"REC" + C::RST; // REC (red )
    } else {
        recStatus = std::wstring(C::GRAY) + L"REC" + C::RST; // REC (gray )
    }

    wchar_t tmp[512];
    swprintf(tmp, 512,
             L" Sort: %s  |  Show: %d  |  Sample: %dms / Refresh: %dms  |  %s  |  "
             L"[Q]uit [R][W][T][S][P] [C]lear [O]Record [M]ini +/- [ ]Speed 1-5",
             sortNames[static_cast<int>(m_sortMode)],
             m_maxDisplay, sampleMs, m_refreshMs,
             recStatus.c_str());
    std::wstring ft = tmp;
    if (static_cast<int>(ft.size()) > innerW - 2)
        ft = ft.substr(0, static_cast<size_t>(innerW) - 2);
    write(cuPos(m_consoleH - 1, 1) + std::wstring(C::CYAN) + BOX_V + C::RST + L" " + ft
          + std::wstring(static_cast<size_t>(std::max(innerW - static_cast<int>(ft.size()) - 1, 0)), L' ')
          + C::CYAN + BOX_V + C::RST);

    // Bottom border
    write(cuPos(m_consoleH, 1) + std::wstring(C::WHITE) + BOX_BL + repeat(BOX_H, innerW) + BOX_BR + C::RST);
}

// ── Overlay mini-window mode ────────────────────────────────────────
bool ConsoleDisplay::enterOverlayMode(DiskMonitor& monitor, Recorder& recorder) {
    // Clear the console screen
    write(std::wstring(CLS) + SHOW_CUR);

    // Start the overlay window
    if (!m_overlay.start(monitor, recorder)) {
        // Failed — redraw full console and continue
        write(std::wstring(CLS) + HIDE_CUR);
        return false;
    }

    // Hide the console window while overlay is active
    HWND hConsole = GetConsoleWindow();
    ShowWindow(hConsole, SW_HIDE);

    // Wait for the overlay to signal switch-back
    while (m_running.load() && !m_overlay.shouldSwitchBack()) {
        // Still check for keyboard input in case user presses Q in console
        if (_kbhit()) {
            int ch = _getch();
            if (ch == 'q' || ch == 'Q' || ch == 27) {
                m_running = false;
                m_overlay.stop();
                ShowWindow(hConsole, SW_SHOW);
                return false; // signal that we're quitting, not switching back
            }
            if (ch == 'm' || ch == 'M') {
                // M pressed again — switch back
                break;
            }
        }

        // Keep pushing samples to recorder if active
        if (recorder.isRecording() && monitor.hasNewData()) {
            monitor.clearNewDataFlag();
            auto procs = monitor.getProcesses(m_sortMode, static_cast<size_t>(m_maxDisplay));
            recorder.enqueueSample(procs);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    m_overlay.stop();
    m_overlay.clearSwitchBackFlag();

    // Restore console window
    ShowWindow(hConsole, SW_SHOW);
    SetForegroundWindow(hConsole);

    // Restore console state
    DWORD mode = m_oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(m_hOut, mode);
    SetConsoleOutputCP(CP_UTF8);
    write(HIDE_CUR);

    return m_running.load();
}

void ConsoleDisplay::exitOverlayMode() {
    m_overlay.stop();
}

// ── SMART Monitor sub-page ───────────────────────────────────────────
bool ConsoleDisplay::enterSmartMonitorMode(DiskMonitor& monitor, Recorder& recorder) {
    // Clear the console screen
    write(std::wstring(CLS) + SHOW_CUR);

    // Hide the console window while SMART page is active
    HWND hConsole = GetConsoleWindow();
    ShowWindow(hConsole, SW_HIDE);

    // Create and show the SMART monitor
    SmartMonitor smartMon;
    smartMon.show();

    // Restore console window
    ShowWindow(hConsole, SW_SHOW);
    SetForegroundWindow(hConsole);

    // Restore console state
    DWORD mode = m_oldOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
    SetConsoleMode(m_hOut, mode);
    SetConsoleOutputCP(CP_UTF8);
    write(HIDE_CUR);

    return m_running.load();
}
