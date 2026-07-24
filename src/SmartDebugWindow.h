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
#include <commctrl.h>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>
#include <deque>

#include "SmartDebug.h"

// ═══════════════════════════════════════════════════════════════════════
//  SmartDebugWindow — Real-time trace viewer for SMART monitor
// ═══════════════════════════════════════════════════════════════════════
//
// Displays:
//   Tab 1: Trace Log    — scrollable list of all trace entries with
//                          thread ID, category, timestamp, function, message
//   Tab 2: Thread Stats — per-thread runtime stats (entry count, errors,
//                          total duration, max duration, last activity)
//   Tab 3: Flow Graph   — ASCII-art timeline of major operations
//
// Hotkeys:
//   F5        — Refresh display
//   Ctrl+C    — Clear all traces
//   Ctrl+L    — Toggle file logging (start/stop writing log file)
//   Space     — Pause/resume auto-scroll

class SmartDebugWindow {
public:
    SmartDebugWindow();
    ~SmartDebugWindow();

    SmartDebugWindow(const SmartDebugWindow&) = delete;
    SmartDebugWindow& operator=(const SmartDebugWindow&) = delete;

    // Create and show the debug window as a modeless child.
    // parentHwnd: the SMART monitor main window handle.
    bool create(HWND parentHwnd, HINSTANCE hInst);

    // Close and destroy the debug window.
    void destroy();

    // Check if window is currently visible.
    bool isVisible() const { return m_hwnd != nullptr && IsWindowVisible(m_hwnd); }

    // Force a refresh of the displayed trace data.
    void refresh();

private:
    // ── Window procedure ──
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // ── Tab handling ──
    void createTabs();
    void switchTab(int index);

    // ── Rendering ──
    void paint(HDC hdc, const RECT& rc);
    void paintTraceLog(HDC hdc, const RECT& rc);
    void paintThreadStats(HDC hdc, const RECT& rc);
    void paintFlowGraph(HDC hdc, const RECT& rc);

    // ── Helpers ──
    void drawHeader(HDC hdc, const RECT& rc);
    std::wstring formatTimestamp(const std::chrono::steady_clock::time_point& tp) const;
    std::wstring categoryName(uint8_t cat) const;
    COLORREF categoryColor(uint8_t cat) const;
    void createFonts();
    void deleteFonts();

    // ── Timer ──
    void startTimer();
    void stopTimer();

    // ── File logging ──
    void toggleFileLogging();

    HWND m_hwnd = nullptr;
    HWND m_parentHwnd = nullptr;
    HINSTANCE m_hInst = nullptr;

    // Tabs
    int m_activeTab = 0;
    static constexpr int TAB_COUNT = 3;
    static constexpr int TAB_HEIGHT = 28;
    static constexpr const wchar_t* TAB_NAMES[] = { L"Trace Log", L"Thread Stats", L"Flow Graph" };

    // Fonts
    HFONT m_hFontMono = nullptr;
    HFONT m_hFontBody = nullptr;

    // Auto-scroll
    bool m_autoScroll = true;

    // File logging state
    bool m_loggingActive = false;

    // Cached trace data for rendering
    std::vector<SmartTraceEntry> m_cachedEntries;
    std::vector<SmartThreadStats> m_cachedThreadStats;
    std::chrono::steady_clock::time_point m_lastRefresh;

    // Refresh timer
    static constexpr UINT_PTR TIMER_ID = 2001;
    static constexpr int REFRESH_INTERVAL_MS = 250; // 4 Hz

    // Window class
    static constexpr const wchar_t* WND_CLASS = L"IOMonitorSmartDebug";
};
