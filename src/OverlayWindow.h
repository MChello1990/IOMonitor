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
#include "Monitor.h"
#include "Recorder.h"
#include "IopsMonitor.h"
#include <string>
#include <atomic>
#include <thread>
#include <mutex>

// ── Overlay mini-window: always-on-top, 25% opaque ──────────────────
// Displays:
//   • Current total disk I/O rate (read + write)
//   • Disk usage percentage (read% / write%)
//   • Recording status indicator
//   • Active process count
//
// Double-click restores to full console mode.

struct OverlaySnapshot {
    double   totalReadRate    = 0.0;
    double   totalWriteRate   = 0.0;
    uint64_t totalReadBytes   = 0;   // cumulative bytes read across all processes
    uint64_t totalWriteBytes  = 0;   // cumulative bytes written across all processes
    int      activeProcesses  = 0;
    int      totalProcesses   = 0;
    bool     isRecording      = false;

    // IOPS & Queue Depth (from IopsMonitor)
    double   readIops         = 0.0;
    double   writeIops        = 0.0;
    double   totalIops        = 0.0;
    double   queueDepth       = 0.0;
    bool     iopsValid        = false;
};

class OverlayWindow {
public:
    OverlayWindow();
    ~OverlayWindow();

    OverlayWindow(const OverlayWindow&) = delete;
    OverlayWindow& operator=(const OverlayWindow&) = delete;

    // Start the overlay window (creates thread + window)
    // Returns false if window creation fails.
    bool start(DiskMonitor& monitor, Recorder& recorder);

    // Stop the overlay, destroy window, join thread.
    void stop();

    // Called by the main loop to signal "switch back to full console".
    // The overlay will destroy itself and set m_switchBack = true.
    bool shouldSwitchBack() const { return m_switchBack.load(); }
    void clearSwitchBackFlag() { m_switchBack = false; }

private:
    static DWORD WINAPI threadProc(LPVOID param);
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    void messageLoop();
    void createWindow();
    void destroyWindow();
    bool updateContent();
    void paint(HDC hdc, const RECT& rc);

    // Formatting
    std::wstring fmtRate(double bps) const;
    std::wstring fmtBytes(uint64_t bytes) const;

    // Transparency control
    void setAlpha(BYTE alpha);

    // Window properties
    static constexpr const wchar_t* WND_CLASS = L"IOMonitorOverlay";

    HWND m_hwnd = nullptr;
    HANDLE m_hThread = nullptr;
    DWORD m_threadId = 0;

    // Data references
    DiskMonitor* m_monitor = nullptr;
    Recorder*    m_recorder = nullptr;

    // IOPS & Queue Depth monitor — started/stopped with overlay
    IopsMonitor  m_iopsMonitor;

    // State
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_switchBack{false};
    bool m_mouseHovering = false;   // track mouse hover for transparency toggle

    // Snapshot for rendering
    mutable std::mutex m_snapMutex;
    OverlaySnapshot m_snapshot;

    // Window dimensions
    int m_width  = 300;
    int m_height = 200;
};
