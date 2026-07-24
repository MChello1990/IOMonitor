#include "SmartDebugWindow.h"
#include <algorithm>
#include <cstdio>
#include <cmath>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#endif

// ═══════════════════════════════════════════════════════════════════════
//  SmartTraceBuffer implementation
// ═══════════════════════════════════════════════════════════════════════

SmartTraceBuffer::SmartTraceBuffer() {
    m_entries.resize(MAX_ENTRIES);
}

void SmartTraceBuffer::addEntry(SmartTraceEntry&& entry) {
    std::lock_guard<std::mutex> lk(m_mutex);

    entry.sequenceId = m_seqCounter.fetch_add(1);
    entry.threadId = GetCurrentThreadId();
    entry.timestamp = std::chrono::steady_clock::now();

    m_entries[m_writeIndex] = entry;
    m_writeIndex = (m_writeIndex + 1) % MAX_ENTRIES;
    m_totalCount.fetch_add(1);

    // Write to file log if active (lock is held, safe to access m_logFile)
    if (m_fileLogging.load()) {
        writeEntryToFile(entry);
    }

    updateThreadStats(m_entries[m_writeIndex == 0 ? MAX_ENTRIES - 1 : m_writeIndex - 1]);
}

void SmartTraceBuffer::addBegin(const char* func, SmartTraceCategory cat, const char* msg) {
    SmartTraceEntry entry;
    entry.function = func;
    entry.category = static_cast<uint32_t>(cat);
    entry.message = msg;
    entry.isBegin = true;
    entry.pairId = m_seqCounter.load();

    // Record active span for this thread
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        ActiveSpan span;
        span.startTime = std::chrono::steady_clock::now();
        span.category = cat;
        span.pairId = entry.pairId;
        m_activeSpans[GetCurrentThreadId()].push_back(span);
    }

    addEntry(std::move(entry));
}

void SmartTraceBuffer::addEnd(const char* func, SmartTraceCategory cat, const char* msg) {
    DWORD tid = GetCurrentThreadId();
    auto now = std::chrono::steady_clock::now();

    SmartTraceEntry entry;
    entry.function = func;
    entry.category = static_cast<uint32_t>(cat);
    entry.message = msg;
    entry.isBegin = false;

    // Find matching begin span for this thread
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto it = m_activeSpans.find(tid);
        if (it != m_activeSpans.end() && !it->second.empty()) {
            auto& span = it->second.back();
            if (span.category == cat) {
                double duration = std::chrono::duration<double, std::milli>(now - span.startTime).count();
                entry.durationMs = duration;
                entry.pairId = span.pairId;
                it->second.pop_back();
            }
        }
    }

    addEntry(std::move(entry));
}

void SmartTraceBuffer::addEvent(const char* func, SmartTraceCategory cat, const char* msg) {
    SmartTraceEntry entry;
    entry.function = func;
    entry.category = static_cast<uint32_t>(cat);
    entry.message = msg;
    addEntry(std::move(entry));
}

void SmartTraceBuffer::updateThreadStats(const SmartTraceEntry& entry) {
    auto& stats = m_threadStats[entry.threadId];
    stats.threadId = entry.threadId;
    stats.totalEntries++;
    stats.lastActivity = entry.timestamp;
    stats.alive = true;
    if (entry.durationMs > 0) {
        stats.totalDurationMs += entry.durationMs;
        if (entry.durationMs > stats.maxDurationMs) {
            stats.maxDurationMs = entry.durationMs;
        }
    }
    if (entry.category == static_cast<uint32_t>(SmartTraceCategory::ERROR_EXCEPTION)) {
        stats.errorCount++;
    }
}

std::vector<SmartTraceEntry> SmartTraceBuffer::snapshot() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<SmartTraceEntry> result;
    result.reserve(MAX_ENTRIES);

    size_t count = std::min(MAX_ENTRIES, static_cast<size_t>(m_totalCount.load()));
    for (size_t i = 0; i < count; ++i) {
        size_t idx = (m_writeIndex + MAX_ENTRIES - count + i) % MAX_ENTRIES;
        if (m_entries[idx].sequenceId > 0) {
            result.push_back(m_entries[idx]);
        }
    }
    return result;
}

std::vector<SmartThreadStats> SmartTraceBuffer::threadStats() const {
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<SmartThreadStats> result;
    result.reserve(m_threadStats.size());
    for (auto& [tid, stats] : m_threadStats) {
        result.push_back(stats);
    }
    // Sort by total entries descending
    std::sort(result.begin(), result.end(),
              [](const SmartThreadStats& a, const SmartThreadStats& b) {
                  return a.totalEntries > b.totalEntries;
              });
    return result;
}

void SmartTraceBuffer::clear() {
    std::lock_guard<std::mutex> lk(m_mutex);
    m_entries.assign(MAX_ENTRIES, SmartTraceEntry{});
    m_writeIndex = 0;
    m_totalCount = 0;
    m_activeSpans.clear();
    m_threadStats.clear();
}

// ── File logging ─────────────────────────────────────────────────────

bool SmartTraceBuffer::startFileLogging() {
    if (m_fileLogging.load()) return true; // Already logging

    // Build file path: <exe_dir>/IO_SMART_Debug_YYYYMMDD_HHMMSS.log
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir(exePath);
    size_t lastSlash = dir.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) dir = dir.substr(0, lastSlash + 1);
    else dir = L".\\";

    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);

    wchar_t name[128];
    swprintf(name, 128, L"IO_SMART_Debug_%04d%02d%02d_%02d%02d%02d.log",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);

    m_logFilePath = dir + name;

    // Open file in UTF-8 mode
    errno_t err = fopen_s(&m_logFile, 
        std::string(m_logFilePath.begin(), m_logFilePath.end()).c_str(), 
        "w");
    if (err != 0 || !m_logFile) return false;

    // Write BOM for UTF-8
    const unsigned char bom[] = { 0xEF, 0xBB, 0xBF };
    fwrite(bom, 1, 3, m_logFile);

    // Write header
    fprintf(m_logFile, "═══════════════════════════════════════════════════════════════\n");
    fprintf(m_logFile, "  IO Monitor — SMART Debug Trace Log\n");
    fprintf(m_logFile, "  Started: %04d-%02d-%02d %02d:%02d:%02d\n",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
            tm.tm_hour, tm.tm_min, tm.tm_sec);
    fprintf(m_logFile, "═══════════════════════════════════════════════════════════════\n");
    fprintf(m_logFile, "\n");
    fprintf(m_logFile, "%-8s %-8s %-12s %-28s %s\n", 
            "Seq", "TID", "Category", "Function", "Message");
    fprintf(m_logFile, "%-8s %-8s %-12s %-28s %s\n",
            "──────", "──────", "──────────", "──────────────────────────", "──────────────────");
    fflush(m_logFile);

    m_fileLogging = true;
    return true;
}

void SmartTraceBuffer::stopFileLogging() {
    m_fileLogging = false;
    if (m_logFile) {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        std::tm tm;
        localtime_s(&tm, &t);

        fprintf(m_logFile, "\n");
        fprintf(m_logFile, "═══════════════════════════════════════════════════════════════\n");
        fprintf(m_logFile, "  Log ended: %04d-%02d-%02d %02d:%02d:%02d\n",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
        fprintf(m_logFile, "  Total entries: %llu\n", m_totalCount.load());
        fprintf(m_logFile, "═══════════════════════════════════════════════════════════════\n");
        fclose(m_logFile);
        m_logFile = nullptr;
    }
}

SmartTraceBuffer::~SmartTraceBuffer() {
    stopFileLogging();
}

void SmartTraceBuffer::writeEntryToFile(const SmartTraceEntry& entry) {
    if (!m_logFile) return;

    // Convert function name to narrow (already narrow)
    // Convert timestamp to system time
    auto steadyNow = entry.timestamp;
    auto sysNow = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(sysNow);
    std::tm tm;
    localtime_s(&tm, &t);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        sysNow.time_since_epoch()).count() % 1000;

    // Category name
    const char* catName = "UNKNOWN";
    switch (static_cast<SmartTraceCategory>(entry.category)) {
    case SmartTraceCategory::THREAD_LIFECYCLE: catName = "THREAD";    break;
    case SmartTraceCategory::DISK_DISCOVERY:   catName = "DISCOVER";  break;
    case SmartTraceCategory::READER_IOCTL:     catName = "IOCTL";     break;
    case SmartTraceCategory::DATA_REFRESH:     catName = "REFRESH";   break;
    case SmartTraceCategory::HEALTH_COMPUTE:   catName = "HEALTH";    break;
    case SmartTraceCategory::RATE_COMPUTE:     catName = "RATE";      break;
    case SmartTraceCategory::ATTRIBUTE_PARSE:  catName = "ATTR";      break;
    case SmartTraceCategory::WINDOW_EVENT:     catName = "WINDOW";    break;
    case SmartTraceCategory::OVERLAY_EVENT:    catName = "OVERLAY";   break;
    case SmartTraceCategory::USER_INTERACTION: catName = "USER";      break;
    case SmartTraceCategory::ERROR_EXCEPTION:  catName = "ERROR";     break;
    default: break;
    }

    // Build timestamp string
    char timestamp[32];
    snprintf(timestamp, sizeof(timestamp), "%02d:%02d:%02d.%03lld",
             tm.tm_hour, tm.tm_min, tm.tm_sec, ms);

    // Write line: Seq TID Timestamp Category Function Message [Dur]
    if (entry.durationMs > 0.0) {
        fprintf(m_logFile, "%-8llu %-8lu %s %-12s %-28s %s [%.2f ms]\n",
                entry.sequenceId, entry.threadId, timestamp,
                catName, entry.function.c_str(), entry.message.c_str(),
                entry.durationMs);
    } else if (entry.isBegin) {
        fprintf(m_logFile, "%-8llu %-8lu %s %-12s %-28s >>> %s\n",
                entry.sequenceId, entry.threadId, timestamp,
                catName, entry.function.c_str(), entry.message.c_str());
    } else {
        fprintf(m_logFile, "%-8llu %-8lu %s %-12s %-28s %s\n",
                entry.sequenceId, entry.threadId, timestamp,
                catName, entry.function.c_str(), entry.message.c_str());
    }
    fflush(m_logFile);
}

// ═══════════════════════════════════════════════════════════════════════
//  SmartDebugWindow implementation
// ═══════════════════════════════════════════════════════════════════════

constexpr const wchar_t* SmartDebugWindow::TAB_NAMES[];

SmartDebugWindow::SmartDebugWindow() {}

SmartDebugWindow::~SmartDebugWindow() {
    destroy();
}

bool SmartDebugWindow::create(HWND parentHwnd, HINSTANCE hInst) {
    m_parentHwnd = parentHwnd;
    m_hInst = hInst;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(DKGRAY_BRUSH);
    wc.lpszClassName = WND_CLASS;
    RegisterClassExW(&wc);

    // Size: 900x500, positioned to the right of the parent window
    RECT parentRc;
    GetWindowRect(parentHwnd, &parentRc);
    int x = parentRc.right + 10;
    int y = parentRc.top;
    int w = 900;
    int h = 500;

    // Ensure it fits on screen
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    if (x + w > screenW) x = std::max(0, screenW - w - 20);

    m_hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        WND_CLASS,
        L"SMART Debug Trace",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        x, y, w, h,
        parentHwnd, nullptr, hInst, this);

    if (!m_hwnd) return false;

    createFonts();
    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    startTimer();

    return true;
}

void SmartDebugWindow::destroy() {
    stopTimer();
    deleteFonts();
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    UnregisterClassW(WND_CLASS, m_hInst);
}

void SmartDebugWindow::refresh() {
    m_cachedEntries = SmartTraceBuffer::instance().snapshot();
    m_cachedThreadStats = SmartTraceBuffer::instance().threadStats();
    m_lastRefresh = std::chrono::steady_clock::now();
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

// ── Fonts ────────────────────────────────────────────────────────────

void SmartDebugWindow::createFonts() {
    m_hFontMono = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    m_hFontBody = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void SmartDebugWindow::deleteFonts() {
    if (m_hFontMono) { DeleteObject(m_hFontMono); m_hFontMono = nullptr; }
    if (m_hFontBody) { DeleteObject(m_hFontBody); m_hFontBody = nullptr; }
}

// ── Timer ────────────────────────────────────────────────────────────

void SmartDebugWindow::startTimer() {
    if (m_hwnd) SetTimer(m_hwnd, TIMER_ID, REFRESH_INTERVAL_MS, nullptr);
}

void SmartDebugWindow::stopTimer() {
    if (m_hwnd) KillTimer(m_hwnd, TIMER_ID);
}

// ── Window procedure ─────────────────────────────────────────────────

LRESULT CALLBACK SmartDebugWindow::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SmartDebugWindow* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SmartDebugWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return 0;
    }

    self = reinterpret_cast<SmartDebugWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_SIZE:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_TIMER:
        if (wp == TIMER_ID) {
            self->refresh();
            return 0;
        }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        self->paint(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);

        // Tab clicks
        if (y >= 0 && y <= TAB_HEIGHT) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            int tabW = rc.right / TAB_COUNT;
            int tabIdx = x / tabW;
            if (tabIdx >= 0 && tabIdx < TAB_COUNT) {
                self->m_activeTab = tabIdx;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    }

    case WM_KEYDOWN:
        switch (wp) {
        case VK_F5:
            self->refresh();
            return 0;
        case 'C':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                SmartTraceBuffer::instance().clear();
                self->m_cachedEntries.clear();
                self->m_cachedThreadStats.clear();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case 'L':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                self->toggleFileLogging();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case VK_SPACE:
            self->m_autoScroll = !self->m_autoScroll;
            return 0;
        }
        break;

    case WM_CLOSE:
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ═══════════════════════════════════════════════════════════════════════
//  PAINT
// ═══════════════════════════════════════════════════════════════════════

void SmartDebugWindow::paint(HDC hdc, const RECT& /*rc*/) {
    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right;
    int h = client.bottom;

    // Double buffering
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(RGB(30, 30, 34));
    FillRect(memDC, &client, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(memDC, TRANSPARENT);

    // Header with tabs
    drawHeader(memDC, client);

    // Tab content area
    RECT contentRc = {0, TAB_HEIGHT, w, h};
    switch (m_activeTab) {
    case 0: paintTraceLog(memDC, contentRc); break;
    case 1: paintThreadStats(memDC, contentRc); break;
    case 2: paintFlowGraph(memDC, contentRc); break;
    }

    // Blit
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

void SmartDebugWindow::drawHeader(HDC hdc, const RECT& rc) {
    int w = rc.right;
    int tabW = w / TAB_COUNT;

    // Tab background
    HBRUSH headerBg = CreateSolidBrush(RGB(40, 42, 50));
    RECT headerRc = {0, 0, w, TAB_HEIGHT};
    FillRect(hdc, &headerRc, headerBg);
    DeleteObject(headerBg);

    SelectObject(hdc, m_hFontBody);

    for (int i = 0; i < TAB_COUNT; ++i) {
        RECT tabRc = {i * tabW, 0, (i + 1) * tabW, TAB_HEIGHT};

        if (i == m_activeTab) {
            // Active tab highlight
            HBRUSH activeBg = CreateSolidBrush(RGB(30, 30, 34));
            FillRect(hdc, &tabRc, activeBg);
            DeleteObject(activeBg);

            // Bottom accent line
            HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(0, 180, 220));
            HPEN oldP = (HPEN)SelectObject(hdc, accentPen);
            MoveToEx(hdc, tabRc.left, TAB_HEIGHT - 1, nullptr);
            LineTo(hdc, tabRc.right, TAB_HEIGHT - 1);
            SelectObject(hdc, oldP);
            DeleteObject(accentPen);

            SetTextColor(hdc, RGB(220, 222, 228));
        } else {
            SetTextColor(hdc, RGB(120, 122, 130));
        }

        // Tab count badge
        wchar_t tabText[64];
        if (i == 0) {
            swprintf(tabText, 64, L"%s  [%zu]", TAB_NAMES[i], m_cachedEntries.size());
        } else if (i == 1) {
            swprintf(tabText, 64, L"%s  [%zu]", TAB_NAMES[i], m_cachedThreadStats.size());
        } else {
            swprintf(tabText, 64, L"%s", TAB_NAMES[i]);
        }

        DrawTextW(hdc, tabText, -1, &tabRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Separator line
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(60, 62, 70));
    HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, 0, TAB_HEIGHT, nullptr);
    LineTo(hdc, w, TAB_HEIGHT);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);
}

// ── Tab 1: Trace Log ─────────────────────────────────────────────────

void SmartDebugWindow::paintTraceLog(HDC hdc, const RECT& rc) {
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int y = rc.top + 2;

    SelectObject(hdc, m_hFontMono);

    // Column headers
    int colX[6] = {4, 60, 100, 230, 530, 750};
    const wchar_t* headers[] = {L"Seq", L"TID", L"Category", L"Function", L"Message", L"Dur(ms)"};

    SetTextColor(hdc, RGB(0, 180, 220));
    for (int i = 0; i < 6; ++i) {
        RECT hdrRc = {colX[i], y, colX[i] + (i < 5 ? colX[i+1] - colX[i] - 8 : 140), y + 18};
        DrawTextW(hdc, headers[i], -1, &hdrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    y += 20;

    // Separator
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(50, 52, 60));
    HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, 4, y, nullptr);
    LineTo(hdc, w - 4, y);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);
    y += 2;

    // Scrollable log entries (most recent first for debugging)
    int rowH = 17;
    int maxRows = (h - (y - rc.top)) / rowH;
    int totalEntries = static_cast<int>(m_cachedEntries.size());
    int startIdx = std::max(0, totalEntries - maxRows);

    for (int i = startIdx; i < totalEntries && y < rc.bottom - rowH; ++i) {
        auto& entry = m_cachedEntries[i];
        int rowY = y + (i - startIdx) * rowH;

        // Alternate row background
        if ((i % 2) == 0) {
            HBRUSH rowBg = CreateSolidBrush(RGB(34, 36, 42));
            RECT rowRc = {0, rowY, w, rowY + rowH};
            FillRect(hdc, &rowRc, rowBg);
            DeleteObject(rowBg);
        }

        // Color based on category
        COLORREF textColor = categoryColor(static_cast<uint8_t>(entry.category));
        if (entry.category == static_cast<uint32_t>(SmartTraceCategory::ERROR_EXCEPTION)) {
            // Error entries get red background
            HBRUSH errBg = CreateSolidBrush(RGB(60, 20, 20));
            RECT errRc = {0, rowY, w, rowY + rowH};
            FillRect(hdc, &errRc, errBg);
            DeleteObject(errBg);
            textColor = RGB(255, 100, 100);
        }

        SetTextColor(hdc, textColor);

        // Seq ID
        wchar_t line[1024];
        swprintf(line, 1024, L"%llu", entry.sequenceId);
        RECT seqRc = {colX[0], rowY, colX[1] - 4, rowY + rowH};
        DrawTextW(hdc, line, -1, &seqRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Thread ID
        swprintf(line, 1024, L"%lu", entry.threadId);
        RECT tidRc = {colX[1], rowY, colX[2] - 4, rowY + rowH};
        DrawTextW(hdc, line, -1, &tidRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Category
        std::wstring catName = categoryName(static_cast<uint8_t>(entry.category));
        RECT catRc = {colX[2], rowY, colX[3] - 4, rowY + rowH};
        DrawTextW(hdc, catName.c_str(), -1, &catRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Function name
        std::wstring funcW;
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, entry.function.c_str(),
                                           static_cast<int>(entry.function.size()), nullptr, 0);
            funcW.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, entry.function.c_str(),
                                static_cast<int>(entry.function.size()), &funcW[0], len);
        }
        RECT funcRc = {colX[3], rowY, colX[4] - 4, rowY + rowH};
        DrawTextW(hdc, funcW.c_str(), -1, &funcRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Message (truncated)
        std::wstring msgW;
        {
            int len = MultiByteToWideChar(CP_UTF8, 0, entry.message.c_str(),
                                           static_cast<int>(entry.message.size()), nullptr, 0);
            msgW.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, entry.message.c_str(),
                                static_cast<int>(entry.message.size()), &msgW[0], len);
        }
        if (msgW.length() > 50) msgW = msgW.substr(0, 48) + L"..";
        SetTextColor(hdc, RGB(180, 182, 190));
        RECT msgRc = {colX[4], rowY, colX[5] - 4, rowY + rowH};
        DrawTextW(hdc, msgW.c_str(), -1, &msgRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Duration
        if (entry.durationMs > 0) {
            COLORREF durColor = entry.durationMs > 100.0 ? RGB(255, 140, 40) : RGB(140, 200, 140);
            SetTextColor(hdc, durColor);
            swprintf(line, 1024, L"%.2f", entry.durationMs);
            RECT durRc = {colX[5], rowY, w - 8, rowY + rowH};
            DrawTextW(hdc, line, -1, &durRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Footer status bar
    {
        RECT statusRc = {0, rc.bottom - 20, w, rc.bottom};
        HBRUSH statusBg = CreateSolidBrush(RGB(40, 42, 50));
        FillRect(hdc, &statusRc, statusBg);
        DeleteObject(statusBg);

        SelectObject(hdc, m_hFontBody);
        SetTextColor(hdc, RGB(140, 142, 150));
        RECT statusText = {8, rc.bottom - 18, w - 8, rc.bottom};
        wchar_t statusBuf[256];
        swprintf(statusBuf, 256, L"Total: %zu entries | Auto-scroll: %s | Log: %s | F5=Refresh  Ctrl+C=Clear  Ctrl+L=Log  Space=Pause",
                 m_cachedEntries.size(),
                 m_autoScroll ? L"ON" : L"OFF",
                 m_loggingActive ? L"REC" : L"OFF");
        DrawTextW(hdc, statusBuf, -1, &statusText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Show log file path if logging
        if (m_loggingActive) {
            std::wstring logPath = SmartTraceBuffer::instance().getLogFilePath();
            RECT pathRc = {8, rc.bottom - 34, w - 8, rc.bottom - 20};
            SetTextColor(hdc, RGB(100, 200, 100));
            wchar_t pathBuf[512];
            swprintf(pathBuf, 512, L"  Log: %s", logPath.c_str());
            DrawTextW(hdc, pathBuf, -1, &pathRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }
}

// ── Tab 2: Thread Stats ──────────────────────────────────────────────

void SmartDebugWindow::paintThreadStats(HDC hdc, const RECT& rc) {
    int w = rc.right - rc.left;
    int y = rc.top + 8;

    SelectObject(hdc, m_hFontMono);

    // Column headers
    SetTextColor(hdc, RGB(0, 180, 220));
    const wchar_t* headers[] = {L"Thread ID", L"Name", L"Entries", L"Errors",
                                 L"Total Dur(s)", L"Max Dur(ms)", L"Last Activity", L"Status"};
    int colW[] = {80, 100, 70, 60, 110, 100, 130, 60};
    int cx = 8;
    for (int i = 0; i < 8; ++i) {
        RECT hdrRc = {cx, y, cx + colW[i], y + 20};
        DrawTextW(hdc, headers[i], -1, &hdrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[i] + 4;
    }
    y += 24;

    // Separator
    HPEN sepPen = CreatePen(PS_SOLID, 1, RGB(50, 52, 60));
    HPEN oldPen = (HPEN)SelectObject(hdc, sepPen);
    MoveToEx(hdc, 8, y, nullptr);
    LineTo(hdc, w - 8, y);
    SelectObject(hdc, oldPen);
    DeleteObject(sepPen);
    y += 4;

    // Thread stats rows
    int rowH = 22;
    for (size_t i = 0; i < m_cachedThreadStats.size() && y < rc.bottom - 30; ++i) {
        auto& stats = m_cachedThreadStats[i];

        if ((i % 2) == 0) {
            HBRUSH rowBg = CreateSolidBrush(RGB(34, 36, 42));
            RECT rowRc = {0, y, w, y + rowH};
            FillRect(hdc, &rowRc, rowBg);
            DeleteObject(rowBg);
        }

        SetTextColor(hdc, RGB(200, 202, 210));
        cx = 8;
        wchar_t buf[128];

        // Thread ID
        swprintf(buf, 128, L"%lu", stats.threadId);
        RECT tidRc = {cx, y, cx + colW[0], y + rowH};
        DrawTextW(hdc, buf, -1, &tidRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[0] + 4;

        // Thread name
        std::wstring nameW;
        if (!stats.threadName.empty()) {
            int len = MultiByteToWideChar(CP_UTF8, 0, stats.threadName.c_str(),
                                           static_cast<int>(stats.threadName.size()), nullptr, 0);
            nameW.resize(len);
            MultiByteToWideChar(CP_UTF8, 0, stats.threadName.c_str(),
                                static_cast<int>(stats.threadName.size()), &nameW[0], len);
        } else {
            nameW = L"Worker";
        }
        RECT nameRc = {cx, y, cx + colW[1], y + rowH};
        DrawTextW(hdc, nameW.c_str(), -1, &nameRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[1] + 4;

        // Entry count
        swprintf(buf, 128, L"%llu", stats.totalEntries);
        RECT entRc = {cx, y, cx + colW[2], y + rowH};
        DrawTextW(hdc, buf, -1, &entRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[2] + 4;

        // Error count
        if (stats.errorCount > 0) SetTextColor(hdc, RGB(255, 100, 100));
        else SetTextColor(hdc, RGB(100, 200, 100));
        swprintf(buf, 128, L"%llu", stats.errorCount);
        RECT errRc = {cx, y, cx + colW[3], y + rowH};
        DrawTextW(hdc, buf, -1, &errRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[3] + 4;

        // Total duration
        SetTextColor(hdc, RGB(200, 202, 210));
        swprintf(buf, 128, L"%.3f", stats.totalDurationMs / 1000.0);
        RECT durRc = {cx, y, cx + colW[4], y + rowH};
        DrawTextW(hdc, buf, -1, &durRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[4] + 4;

        // Max duration
        swprintf(buf, 128, L"%.2f", stats.maxDurationMs);
        RECT maxRc = {cx, y, cx + colW[5], y + rowH};
        DrawTextW(hdc, buf, -1, &maxRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[5] + 4;

        // Last activity
        std::wstring timeStr = formatTimestamp(stats.lastActivity);
        RECT timeRc = {cx, y, cx + colW[6], y + rowH};
        DrawTextW(hdc, timeStr.c_str(), -1, &timeRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        cx += colW[6] + 4;

        // Alive status
        SetTextColor(hdc, stats.alive ? RGB(100, 220, 100) : RGB(140, 140, 140));
        RECT aliveRc = {cx, y, cx + colW[7], y + rowH};
        DrawTextW(hdc, stats.alive ? L"ALIVE" : L"DEAD", -1, &aliveRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        y += rowH;
    }
}

// ── Tab 3: Flow Graph ────────────────────────────────────────────────

void SmartDebugWindow::paintFlowGraph(HDC hdc, const RECT& rc) {
    int w = rc.right - rc.left;
    int y = rc.top + 8;

    SelectObject(hdc, m_hFontMono);
    SetTextColor(hdc, RGB(200, 202, 210));

    // Build a simplified ASCII timeline grouped by thread
    // Show major operations: enumerateDisks, createReader, readIdentity, readAttributes, computeHealth, paint

    static const char* majorOps[] = {
        "enumerateDisks", "createReaderForDisk", "readIdentity",
        "readAttributes", "readThresholds", "computeHealth",
        "computeRates", "refreshSmartData", "paint", "paintOverlay"
    };

    // Group entries by thread
    std::unordered_map<DWORD, std::vector<SmartTraceEntry>> threadEntries;
    for (auto& entry : m_cachedEntries) {
        // Only show major operations
        for (const char* op : majorOps) {
            if (entry.function == op) {
                threadEntries[entry.threadId].push_back(entry);
                break;
            }
        }
    }

    if (threadEntries.empty()) {
        RECT emptyRc = {8, y + 40, w - 8, y + 60};
        DrawTextW(hdc, L"No major operations traced yet. Wait for data refresh or disk enumeration.", -1,
                  &emptyRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        return;
    }

    // For each thread, draw a horizontal timeline
    const int laneH = 40;
    int threadIdx = 0;
    std::vector<DWORD> threadIds;
    for (auto& [tid, entries] : threadEntries) {
        threadIds.push_back(tid);
    }
    std::sort(threadIds.begin(), threadIds.end());

    for (DWORD tid : threadIds) {
        auto& entries = threadEntries[tid];
        int laneY = y + threadIdx * laneH;

        // Thread label
        SetTextColor(hdc, RGB(0, 180, 220));
        wchar_t tidLabel[32];
        swprintf(tidLabel, 32, L"TID:%lu", tid);
        RECT labelRc = {4, laneY, 100, laneY + 20};
        DrawTextW(hdc, tidLabel, -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Timeline background
        int timelineX = 100;
        int timelineW = w - timelineX - 8;
        HBRUSH tlBg = CreateSolidBrush(RGB(40, 44, 52));
        RECT tlRc = {timelineX, laneY + 22, timelineX + timelineW, laneY + laneH - 4};
        FillRect(hdc, &tlRc, tlBg);
        DeleteObject(tlBg);

        // Draw timeline border
        HPEN tlPen = CreatePen(PS_SOLID, 1, RGB(60, 62, 70));
        HPEN oldP = (HPEN)SelectObject(hdc, tlPen);
        HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, nullB);
        Rectangle(hdc, tlRc.left, tlRc.top, tlRc.right, tlRc.bottom);
        SelectObject(hdc, oldP); SelectObject(hdc, oldB);
        DeleteObject(tlPen);

        // Draw operation blocks
        if (!entries.empty()) {
            auto firstTime = entries.front().timestamp;
            auto lastTime = entries.back().timestamp;
            double totalSpan = std::chrono::duration<double>(lastTime - firstTime).count();
            if (totalSpan <= 0) totalSpan = 1.0;

            int blockH = 12;
            for (auto& entry : entries) {
                double relPos = std::chrono::duration<double>(entry.timestamp - firstTime).count() / totalSpan;
                int bx = timelineX + static_cast<int>(relPos * timelineW) + 2;
                int by = laneY + 24 + (threadIdx % 3) * (blockH + 2);

                COLORREF blockColor = categoryColor(static_cast<uint8_t>(entry.category));
                HBRUSH blockBr = CreateSolidBrush(blockColor);
                int blockW = std::max(4, static_cast<int>(entry.durationMs / totalSpan * timelineW / 10));
                if (blockW > 100) blockW = 100;

                RECT blockRc = {bx, by, bx + blockW, by + blockH};
                FillRect(hdc, &blockRc, blockBr);
                DeleteObject(blockBr);

                // Label
                std::wstring opName;
                {
                    int len = MultiByteToWideChar(CP_UTF8, 0, entry.function.c_str(),
                                                   static_cast<int>(entry.function.size()), nullptr, 0);
                    opName.resize(len);
                    MultiByteToWideChar(CP_UTF8, 0, entry.function.c_str(),
                                        static_cast<int>(entry.function.size()), &opName[0], len);
                }
                SetTextColor(hdc, RGB(200, 202, 210));
                RECT opLabelRc = {bx + blockW + 4, by, bx + blockW + 180, by + blockH};
                DrawTextW(hdc, opName.c_str(), -1, &opLabelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        threadIdx++;
        if (laneY + laneH > rc.bottom - 30) break;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════════════

std::wstring SmartDebugWindow::formatTimestamp(const std::chrono::steady_clock::time_point& tp) const {
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    int seconds = static_cast<int>((ms / 1000) % 60);
    int minutes = static_cast<int>((ms / 60000) % 60);
    int hours = static_cast<int>((ms / 3600000) % 24);
    int millis = static_cast<int>(ms % 1000);

    wchar_t buf[32];
    swprintf(buf, 32, L"%02d:%02d:%02d.%03d", hours, minutes, seconds, millis);
    return buf;
}

std::wstring SmartDebugWindow::categoryName(uint8_t cat) const {
    switch (static_cast<SmartTraceCategory>(cat)) {
    case SmartTraceCategory::THREAD_LIFECYCLE: return L"THREAD";
    case SmartTraceCategory::DISK_DISCOVERY:   return L"DISCOVER";
    case SmartTraceCategory::READER_IOCTL:     return L"IOCTL";
    case SmartTraceCategory::DATA_REFRESH:     return L"REFRESH";
    case SmartTraceCategory::HEALTH_COMPUTE:   return L"HEALTH";
    case SmartTraceCategory::RATE_COMPUTE:     return L"RATE";
    case SmartTraceCategory::ATTRIBUTE_PARSE:  return L"ATTR";
    case SmartTraceCategory::WINDOW_EVENT:     return L"WINDOW";
    case SmartTraceCategory::OVERLAY_EVENT:    return L"OVERLAY";
    case SmartTraceCategory::USER_INTERACTION: return L"USER";
    case SmartTraceCategory::ERROR_EXCEPTION:  return L"ERROR";
    default: return L"UNKNOWN";
    }
}

COLORREF SmartDebugWindow::categoryColor(uint8_t cat) const {
    switch (static_cast<SmartTraceCategory>(cat)) {
    case SmartTraceCategory::THREAD_LIFECYCLE: return RGB(180, 160, 255);
    case SmartTraceCategory::DISK_DISCOVERY:   return RGB(100, 200, 255);
    case SmartTraceCategory::READER_IOCTL:     return RGB(255, 200, 100);
    case SmartTraceCategory::DATA_REFRESH:     return RGB(100, 255, 180);
    case SmartTraceCategory::HEALTH_COMPUTE:   return RGB(255, 220, 80);
    case SmartTraceCategory::RATE_COMPUTE:     return RGB(180, 220, 255);
    case SmartTraceCategory::ATTRIBUTE_PARSE:  return RGB(200, 180, 255);
    case SmartTraceCategory::WINDOW_EVENT:     return RGB(180, 200, 200);
    case SmartTraceCategory::OVERLAY_EVENT:    return RGB(255, 180, 200);
    case SmartTraceCategory::USER_INTERACTION: return RGB(200, 255, 200);
    case SmartTraceCategory::ERROR_EXCEPTION:  return RGB(255, 80, 80);
    default: return RGB(180, 180, 180);
    }
}

// ── File logging toggle ──────────────────────────────────────────────

void SmartDebugWindow::toggleFileLogging() {
    auto& buf = SmartTraceBuffer::instance();
    if (m_loggingActive) {
        buf.stopFileLogging();
        m_loggingActive = false;
    } else {
        if (buf.startFileLogging()) {
            m_loggingActive = true;
            // Also write existing entries from buffer to the file
            auto entries = buf.snapshot();
            // They were already written during addEntry, so no need to re-write
        }
    }
}
