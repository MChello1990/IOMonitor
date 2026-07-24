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
#include <windowsx.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <deque>
#include <cstdint>

#include "SmartDataModel.h"
#include "SmartReaderBase.h"
#include "SmartDebug.h"

// ── Forward declarations ─────────────────────────────────────────────
class SmartReaderAta;
class SmartReaderNvme;
class SmartReaderScsi;
class SmartDebugWindow;

// ── SmartMonitor: Main SMART health monitor page (GUI window) ────────

class SmartMonitor {
public:
    SmartMonitor();
    ~SmartMonitor();

    SmartMonitor(const SmartMonitor&) = delete;
    SmartMonitor& operator=(const SmartMonitor&) = delete;

    // Create and show the monitor window. Returns when window closes.
    // Returns true if user wants to go back to main console, false if quit.
    bool show();

private:
    // ── Window procedures ──
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static DWORD WINAPI refreshThreadProc(LPVOID param);

    // ── Initialization ──
    bool createMainWindow();
    bool createOverlayWindow();
    void registerWindowClasses();

    // ── Disk enumeration & detection ──
    void enumerateDisks();
    std::unique_ptr<SmartReaderBase> createReaderForDisk(uint32_t diskNumber);

    // ── Data refresh ──
    void refreshSmartData();
    bool readSmartDataForDisk(uint32_t diskNumber, SmartDataSnapshot& snapshot);
    void computeHealth(SmartDataSnapshot& snapshot);
    void computeRates(SmartDataSnapshot& snapshot);

    // ── Rendering ──
    void paint(HDC hdc, const RECT& rc);
    void paintOverlay(HDC hdc, const RECT& rc);

    void drawDiskSelector(HDC hdc, int& y, int w);
    void drawDiskOverview(HDC hdc, int& y, int w);
    void drawMetricCards(HDC hdc, int y, int x, int cardW);
    void drawCharts(HDC hdc, int y, int x, int chartW);
    void drawDetailedAttributes(HDC hdc, int y, int x, int panelW);
    void drawFooter(HDC hdc, const RECT& rc);

    void drawRingGauge(HDC hdc, int cx, int cy, int radius, double percent, const wchar_t* label);
    void drawMiniLineChart(HDC hdc, int x, int y, int w, int h,
                           const std::vector<TempPoint>& data, double maxVal, COLORREF color);
    void drawMiniBarChart(HDC hdc, int x, int y, int w, int h,
                          const std::vector<double>& data, COLORREF color);
    void drawSparkLine(HDC hdc, int x, int y, int w, int h,
                       const std::deque<double>& data, double maxVal, COLORREF color);

    // ── Helpers ──
    std::wstring fmtBytesSmart(uint64_t bytes) const;
    std::wstring fmtRateMBps(double mbps) const;
    std::wstring fmtHours(uint64_t hours) const;
    std::wstring fmtTemperature(double celsius) const;
    COLORREF healthColor(double percent) const;
    COLORREF tempColor(double celsius) const;
    void createFonts();
    void deleteFonts();

    // String conversion
    static std::wstring ataToWide(const std::string& s);

    // ── Overlay ──
    void toggleOverlay();
    void updateOverlayContent();
    void startRefreshThread();
    void stopRefreshThread();
    void onRefreshNow();

    // ── Debug window ──
    void toggleDebugWindow();
    void updateDebugWindow();

    // ── State ──
    HWND m_hwnd = nullptr;
    HWND m_overlayHwnd = nullptr;
    HINSTANCE m_hInst = nullptr;

    // Fonts
    HFONT m_hFontTitle = nullptr;
    HFONT m_hFontBody = nullptr;
    HFONT m_hFontSmall = nullptr;
    HFONT m_hFontMono = nullptr;
    HFONT m_hFontOverlay = nullptr;

    // Disk list
    struct DiskEntry {
        DiskIdentity identity;
        SmartDataSnapshot snapshot;
        bool smartAvailable = false;
        std::string errorMessage;
    };
    std::vector<DiskEntry> m_disks;
    size_t m_selectedDiskIndex = 0;
    std::mutex m_dataMutex;

    // Refresh settings
    std::atomic<int> m_refreshIntervalSec{120};  // Default 120 seconds
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_inOverlay{false};
    std::atomic<bool> m_needsRefresh{false};
    std::atomic<bool> m_backgroundMode{false};   // Window minimized/hidden
    HANDLE m_hRefreshThread = nullptr;

    // Chart data (historical)
    std::deque<TempPoint> m_tempHistory;
    std::deque<double> m_healthHistory;
    std::chrono::steady_clock::time_point m_sessionStart;

    // Previous snapshot for rate calculation
    SmartDataSnapshot m_prevSnapshot;
    std::chrono::steady_clock::time_point m_prevSampleTime;

    // Overlay dimensions
    int m_overlayW = 280;
    int m_overlayH = 200;

    // Window class names
    static constexpr const wchar_t* MAIN_CLASS = L"IOMonitorSmartPage";
    static constexpr const wchar_t* OVERLAY_CLASS = L"IOMonitorSmartOverlay";

    // Default high temperature threshold (°C)
    double m_highTempThreshold = 60.0;

    // Debug window
    std::unique_ptr<SmartDebugWindow> m_debugWindow;
    bool m_debugWindowVisible = false;
};
