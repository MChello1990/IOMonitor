#include "OverlayWindow.h"
#include <windowsx.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <chrono>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")

// ── Formatting helpers (same style as Display.cpp) ──────────────────
std::wstring OverlayWindow::fmtRate(double bps) const {
    if (bps < 0.5) return L"0.00  B/s";
    const wchar_t* u[] = { L" B/s", L"KB/s", L"MB/s", L"GB/s" };
    int i = 0; double v = bps;
    while (v >= 1024.0 && i < 3) { v /= 1024.0; i++; }
    wchar_t b[32];
    swprintf(b, 32, L"%.2f %s", v, u[i]);
    return b;
}

std::wstring OverlayWindow::fmtBytes(uint64_t bytes) const {
    if (bytes == 0) return L"0  B";
    const wchar_t* u[] = { L" B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0; double v = static_cast<double>(bytes);
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    wchar_t b[32];
    if (v >= 100.0)      swprintf(b, 32, L"%.0f %s", v, u[i]);
    else if (v >= 10.0)  swprintf(b, 32, L"%.1f %s", v, u[i]);
    else                 swprintf(b, 32, L"%.2f %s", v, u[i]);
    return b;
}

void OverlayWindow::setAlpha(BYTE alpha) {
    if (m_hwnd) {
        SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), alpha, LWA_ALPHA);
    }
}

// ── Constructor / Destructor ────────────────────────────────────────
OverlayWindow::OverlayWindow() {}
OverlayWindow::~OverlayWindow() { stop(); }

// ── Start ───────────────────────────────────────────────────────────
bool OverlayWindow::start(DiskMonitor& monitor, Recorder& recorder) {
    if (m_running.load()) return true;

    m_monitor   = &monitor;
    m_recorder  = &recorder;
    m_switchBack = false;
    m_running    = true;

    m_hThread = CreateThread(nullptr, 0, threadProc, this, 0, &m_threadId);
    if (!m_hThread) {
        m_running = false;
        return false;
    }
    return true;
}

// ── Stop ────────────────────────────────────────────────────────────
void OverlayWindow::stop() {
    m_running = false;

    // Post WM_CLOSE to the window to unblock GetMessage
    if (m_hwnd) {
        PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
    }

    if (m_hThread) {
        WaitForSingleObject(m_hThread, 3000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }

    m_monitor  = nullptr;
    m_recorder = nullptr;
}

// ── Thread proc ─────────────────────────────────────────────────────
DWORD WINAPI OverlayWindow::threadProc(LPVOID param) {
    auto* self = static_cast<OverlayWindow*>(param);
    self->createWindow();
    if (self->m_hwnd) {
        self->messageLoop();
    }
    return 0;
}

// ── Register class + create window ──────────────────────────────────
void OverlayWindow::createWindow() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = wndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = WND_CLASS;

    // Allow re-registration if class already exists (e.g., after switch-back)
    RegisterClassExW(&wc);

    // Position: bottom-right corner of primary monitor
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = screenW - m_width - 20;
    int y = screenH - m_height - 60;

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        WND_CLASS,
        L"IO Monitor",
        WS_POPUP,
        x, y, m_width, m_height,
        nullptr, nullptr, hInst, this);

    if (!m_hwnd) return;

    // Set transparency: 25% opacity = 64 alpha (0-255)
    SetLayeredWindowAttributes(m_hwnd, RGB(0, 0, 0), 64, LWA_ALPHA);

    ShowWindow(m_hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_hwnd);
}

// ── Message loop ────────────────────────────────────────────────────
void OverlayWindow::messageLoop() {
    // Set a timer for periodic refresh (every 500ms)
    // Use a unique timer ID via a member to avoid collisions
    const UINT_PTR TIMER_ID = 1001;
    SetTimer(m_hwnd, TIMER_ID, 500, nullptr);

    MSG msg{};
    while (m_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    destroyWindow();
}

// ── Destroy window ──────────────────────────────────────────────────
void OverlayWindow::destroyWindow() {
    if (m_hwnd) {
        KillTimer(m_hwnd, 1001);
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(WND_CLASS, GetModuleHandleW(nullptr));
}

// ── Update snapshot from monitor ────────────────────────────────────
// Returns true if any data value changed, false if unchanged (to skip repaint).
bool OverlayWindow::updateContent() {
    if (!m_monitor) return false;

    auto stats = m_monitor->getSystemStats();
    bool recording = m_recorder ? m_recorder->isRecording() : false;

    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(m_snapMutex);

        if (m_snapshot.totalReadRate   != stats.physicalDiskReadRate  ||
            m_snapshot.totalWriteRate  != stats.physicalDiskWriteRate ||
            m_snapshot.totalReadBytes  != stats.physicalDiskReadBytes ||
            m_snapshot.totalWriteBytes != stats.physicalDiskWriteBytes ||
            m_snapshot.activeProcesses != stats.activeProcessCount    ||
            m_snapshot.totalProcesses  != stats.totalProcessCount     ||
            m_snapshot.isRecording     != recording) {
            changed = true;
        }

        m_snapshot.totalReadRate   = stats.physicalDiskReadRate;
        m_snapshot.totalWriteRate  = stats.physicalDiskWriteRate;
        m_snapshot.totalReadBytes  = stats.physicalDiskReadBytes;
        m_snapshot.totalWriteBytes = stats.physicalDiskWriteBytes;
        m_snapshot.activeProcesses = stats.activeProcessCount;
        m_snapshot.totalProcesses  = stats.totalProcessCount;
        m_snapshot.isRecording     = recording;
    }

    return changed;
}

// ── Paint ───────────────────────────────────────────────────────────
void OverlayWindow::paint(HDC hdc, const RECT& rc) {
    OverlaySnapshot snap;
    {
        std::lock_guard<std::mutex> lk(m_snapMutex);
        snap = m_snapshot;
    }

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    // Create offscreen buffer for flicker-free rendering
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // ── Background ──
    HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 22));
    FillRect(memDC, &rc, bgBrush);
    DeleteObject(bgBrush);

    // ── Border ──
    HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(60, 63, 65));
    HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
    HBRUSH nullBrush = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBr = (HBRUSH)SelectObject(memDC, nullBrush);
    Rectangle(memDC, 0, 0, w, h);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBr);
    DeleteObject(borderPen);

    // ── Fonts ──
    HFONT hFontTitle = CreateFontW(18, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    HFONT hFontBody = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    HFONT hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");

    SetBkMode(memDC, TRANSPARENT);

    // ── Title bar ──
    {
        SetTextColor(memDC, RGB(0, 200, 220)); // cyan
        SelectObject(memDC, hFontTitle);
        RECT titleRc = {8, 4, w - 8, 28};
        DrawTextW(memDC, L"IO Monitor", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Recording indicator
        if (snap.isRecording) {
            SetTextColor(memDC, RGB(255, 60, 60)); // red dot
            SelectObject(memDC, hFontSmall);
            RECT recRc = {w - 70, 6, w - 8, 26};
            DrawTextW(memDC, L"\u25CF REC", -1, &recRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }

        // Close button hint
        SetTextColor(memDC, RGB(100, 100, 105));
        SelectObject(memDC, hFontSmall);
        RECT hintRc = {8, 24, w - 8, 40};
        DrawTextW(memDC, L"double-click to restore", -1, &hintRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ── Separator ──
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(50, 53, 55));
    SelectObject(memDC, sepPen);
    MoveToEx(memDC, 8, 44, nullptr);
    LineTo(memDC, w - 8, 44);
    DeleteObject(sepPen);

    // ── Disk I/O data ──
    // Layout: Total (cumulative bytes) / Read rate / Write rate
    uint64_t totalBytes = snap.totalReadBytes + snap.totalWriteBytes;

    {
        SelectObject(memDC, hFontBody);

        // Total (cumulative I/O bytes)
        SetTextColor(memDC, RGB(180, 180, 185));
        RECT labelRc = {12, 48, 70, 68};
        DrawTextW(memDC, L"Total:", -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, RGB(220, 220, 255));
        RECT totalRc = {72, 48, w - 12, 68};
        std::wstring totalStr = fmtBytes(totalBytes);
        DrawTextW(memDC, totalStr.c_str(), -1, &totalRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Read rate
        SetTextColor(memDC, RGB(180, 180, 185));
        RECT rLabelRc = {12, 70, 70, 90};
        DrawTextW(memDC, L"Read:", -1, &rLabelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, RGB(80, 180, 255));
        RECT rValRc = {72, 70, w - 12, 90};
        std::wstring readStr = fmtRate(snap.totalReadRate);
        DrawTextW(memDC, readStr.c_str(), -1, &rValRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Write rate
        SetTextColor(memDC, RGB(180, 180, 185));
        RECT wLabelRc = {12, 92, 70, 112};
        DrawTextW(memDC, L"Write:", -1, &wLabelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, RGB(255, 160, 60));
        RECT wValRc = {72, 92, w - 12, 112};
        std::wstring writeStr = fmtRate(snap.totalWriteRate);
        DrawTextW(memDC, writeStr.c_str(), -1, &wValRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // ── Cleanup ──
    SelectObject(memDC, hFontTitle);
    DeleteObject(hFontTitle);
    SelectObject(memDC, hFontBody);
    DeleteObject(hFontBody);
    SelectObject(memDC, hFontSmall);
    DeleteObject(hFontSmall);

    // Blit to screen
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

// ── Window procedure ────────────────────────────────────────────────
LRESULT CALLBACK OverlayWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        // Request initial WM_MOUSELEAVE tracking
        {
            TRACKMOUSEEVENT tme = { sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_TIMER: {
        // Timer ID 1001: data refresh (500ms)
        // wp holds the timer ID; match against our known ID
        if (wp == 1001 && self && self->m_running.load()) {
            if (self->updateContent()) {
                // Data actually changed — force immediate repaint
                InvalidateRect(hwnd, nullptr, FALSE);
                UpdateWindow(hwnd);
            }
        }
        return 0;
    }

    case WM_PAINT: {
        if (self) {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            self->paint(hdc, ps.rcPaint);
            EndPaint(hwnd, &ps);
        }
        return 0;
    }

    case WM_ERASEBKGND:
        return 1; // skip default erase to reduce flicker

    // ── Mouse hover tracking (client-area only, no NC interference) ──
    case WM_MOUSEMOVE: {
        if (self) {
            if (!self->m_mouseHovering) {
                self->m_mouseHovering = true;
                self->setAlpha(191); // 75% opacity = 191/255
            }
            // Re-request WM_MOUSELEAVE on every move to keep tracking alive
            TRACKMOUSEEVENT tme = { sizeof(tme) };
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
        }
        return 0;
    }

    case WM_MOUSELEAVE: {
        if (self) {
            self->m_mouseHovering = false;
            self->setAlpha(64); // restore 25% opacity
        }
        return 0;
    }

    // ── Left-button down: distinguish drag (title bar) vs click (data area) ──
    case WM_LBUTTONDOWN: {
        if (self) {
            int y = GET_Y_LPARAM(lp);
            if (y >= 0 && y < 44) {
                // Title bar — initiate window drag via system
                // Release capture first, then delegate to NC drag
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            // Data area — let WM_LBUTTONUP handle the click action
        }
        break; // fall through to DefWindowProc for data area
    }

    case WM_LBUTTONUP: {
        // Single left-click in data area: restore full console
        if (self) {
            int y = GET_Y_LPARAM(lp);
            if (y >= 44) {
                self->m_switchBack = true;
                self->m_running = false;
                DestroyWindow(hwnd);
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONDBLCLK:
        // Double-click anywhere: switch back to full console
        if (self) {
            self->m_switchBack = true;
            self->m_running = false;
            DestroyWindow(hwnd);
        }
        return 0;

    case WM_RBUTTONUP: {
        // Right-click: context menu to switch back or quit
        if (self) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Restore Full Console");
            AppendMenuW(menu, MF_STRING, 2, L"Quit IO Monitor");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) {
                self->m_switchBack = true;
                self->m_running = false;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            } else if (cmd == 2) {
                self->m_switchBack = false; // will quit entirely
                self->m_running = false;
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
        }
        return 0;
    }

    case WM_NCHITTEST: {
        // Do NOT return HTCAPTION from here — it would steal mouse events
        // from the client area. Instead, drag is handled in WM_LBUTTONDOWN.
        LRESULT hit = DefWindowProcW(hwnd, msg, wp, lp);
        // Always return HTCLIENT for our client area to keep mouse messages
        // in the client queue (WM_MOUSEMOVE, WM_LBUTTONDOWN, etc.)
        return hit;
    }

    case WM_CLOSE:
        if (self) self->m_running = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (self) self->m_hwnd = nullptr;
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}
