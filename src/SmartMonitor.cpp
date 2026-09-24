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

#include "SmartMonitor.h"
#include "SmartCdiAdapter.h"
#include "SmartDebugWindow.h"
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <commctrl.h>

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "msimg32.lib")
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ── Color scheme ─────────────────────────────────────────────────────
namespace SmartColors {
    constexpr COLORREF BG_DARK       = RGB(18, 18, 20);
    constexpr COLORREF BG_CARD       = RGB(28, 28, 32);
    constexpr COLORREF BG_HEADER     = RGB(22, 22, 26);
    constexpr COLORREF BORDER        = RGB(50, 52, 58);
    constexpr COLORREF TEXT_PRIMARY  = RGB(220, 222, 228);
    constexpr COLORREF TEXT_SECONDARY= RGB(140, 142, 150);
    constexpr COLORREF TEXT_DIM      = RGB(90, 92, 100);
    constexpr COLORREF ACCENT_CYAN   = RGB(0, 200, 220);
    constexpr COLORREF ACCENT_BLUE   = RGB(70, 150, 255);
    constexpr COLORREF HEALTH_GREEN  = RGB(80, 220, 80);
    constexpr COLORREF HEALTH_YELLOW = RGB(220, 200, 40);
    constexpr COLORREF HEALTH_ORANGE = RGB(255, 150, 30);
    constexpr COLORREF HEALTH_RED    = RGB(255, 50, 50);
    constexpr COLORREF TEMP_COLD     = RGB(70, 180, 255);
    constexpr COLORREF TEMP_WARM     = RGB(255, 180, 50);
    constexpr COLORREF TEMP_HOT      = RGB(255, 60, 60);
    constexpr COLORREF READ_COLOR    = RGB(80, 160, 255);
    constexpr COLORREF WRITE_COLOR   = RGB(255, 140, 40);
    constexpr COLORREF CHART_LINE    = RGB(0, 210, 230);
    constexpr COLORREF CHART_FILL    = RGB(0, 180, 200);
}

// ── Constructor / Destructor ─────────────────────────────────────────

SmartMonitor::SmartMonitor() {
    m_hInst = GetModuleHandleW(nullptr);
    m_sessionStart = std::chrono::steady_clock::now();
}

SmartMonitor::~SmartMonitor() {
    stopRefreshThread();
    deleteFonts();
}

// ── Window class registration ────────────────────────────────────────

void SmartMonitor::registerWindowClasses() {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = m_hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = CreateSolidBrush(SmartColors::BG_DARK);

    // Main window class
    wc.lpszClassName = MAIN_CLASS;
    wc.lpfnWndProc = wndProc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    // Overlay window class
    wc.lpszClassName = OVERLAY_CLASS;
    wc.lpfnWndProc = overlayWndProc;
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    RegisterClassExW(&wc);
}

// ── Show the monitor ─────────────────────────────────────────────────

bool SmartMonitor::show() {
    SMART_TRACE_BEGIN("SmartMonitor::show", SmartTraceCategory::THREAD_LIFECYCLE, "SMART Monitor initializing");

    registerWindowClasses();
    createFonts();

    if (!createMainWindow()) {
        SMART_TRACE_EVENT("SmartMonitor::show", SmartTraceCategory::ERROR_EXCEPTION, "Failed to create main window");
        return false;
    }

    SMART_TRACE_EVENT("SmartMonitor::show", SmartTraceCategory::WINDOW_EVENT, "Main window created");

    enumerateDisks();
    startRefreshThread();

    SMART_TRACE_EVENT("SmartMonitor::show", SmartTraceCategory::THREAD_LIFECYCLE, "Refresh thread started");

    // Do an initial refresh immediately
    refreshSmartData();

    // Drain any stale messages (e.g. WM_QUIT from a previous session)
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Message loop
    while (m_running.load() && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    SMART_TRACE_EVENT("SmartMonitor::show", SmartTraceCategory::THREAD_LIFECYCLE, "Message loop exited, shutting down");

    stopRefreshThread();

    // Stop file logging and close debug window
    SmartTraceBuffer::instance().stopFileLogging();
    if (m_debugWindow) {
        m_debugWindow->destroy();
        m_debugWindow.reset();
    }

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }

    if (m_overlayHwnd) {
        DestroyWindow(m_overlayHwnd);
        m_overlayHwnd = nullptr;
    }

    UnregisterClassW(MAIN_CLASS, m_hInst);
    UnregisterClassW(OVERLAY_CLASS, m_hInst);

    SMART_TRACE_END("SmartMonitor::show", SmartTraceCategory::THREAD_LIFECYCLE, "SMART Monitor shut down");
    return true;
}

// ── Create main window ───────────────────────────────────────────────

bool SmartMonitor::createMainWindow() {
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int w = std::min(1280, screenW - 40);
    int h = std::min(820, screenH - 60);

    m_hwnd = CreateWindowExW(
        0,
        MAIN_CLASS,
        L"SMART Disk Health Monitor",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        (screenW - w) / 2, (screenH - h) / 2,
        w, h,
        nullptr, nullptr, m_hInst, this);

    if (!m_hwnd) return false;

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);
    return true;
}

// ── Create overlay window ────────────────────────────────────────────

bool SmartMonitor::createOverlayWindow() {
    if (m_overlayHwnd) return true;

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = screenW - m_overlayW - 20;
    int y = screenH - m_overlayH - 60;

    m_overlayHwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        OVERLAY_CLASS,
        L"SMART Monitor",
        WS_POPUP | WS_CLIPCHILDREN,
        x, y, m_overlayW, m_overlayH,
        nullptr, nullptr, m_hInst, this);

    if (!m_overlayHwnd) return false;

    ShowWindow(m_overlayHwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(m_overlayHwnd);
    return true;
}

// ── Font creation ────────────────────────────────────────────────────

void SmartMonitor::createFonts() {
    m_hFontTitle = CreateFontW(20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontBody = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontSmall = CreateFontW(12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    m_hFontMono = CreateFontW(14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Consolas");
    m_hFontOverlay = CreateFontW(13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void SmartMonitor::deleteFonts() {
    if (m_hFontTitle)  { DeleteObject(m_hFontTitle); m_hFontTitle = nullptr; }
    if (m_hFontBody)   { DeleteObject(m_hFontBody); m_hFontBody = nullptr; }
    if (m_hFontSmall)  { DeleteObject(m_hFontSmall); m_hFontSmall = nullptr; }
    if (m_hFontMono)   { DeleteObject(m_hFontMono); m_hFontMono = nullptr; }
    if (m_hFontOverlay){ DeleteObject(m_hFontOverlay); m_hFontOverlay = nullptr; }
}

// ── Disk enumeration ─────────────────────────────────────────────────

namespace {

// Trace messages take narrow strings; the CDI layer speaks std::wstring.
std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int len = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();
    std::string out(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

} // anonymous namespace

void SmartMonitor::enumerateDisks() {
    SMART_TRACE_SCOPE("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, "Starting disk enumeration");

    m_disks.clear();
    m_selectedDiskIndex = 0;

    if (!m_cdi) m_cdi = std::make_unique<cdi::CAtaSmart>();

    // The CrystalDiskInfo port enumerates \\\\.\\PhysicalDriveN, works out the
    // bus type and reads S.M.A.R.T. in a single pass, replacing what used to
    // be the per-disk reader factory plus its bus-type switch here.
    m_cdi->Init(FALSE);

    const DWORD count = m_cdi->GetDiskCount();
    if (count == 0) {
        SMART_TRACE_EVENT("enumerateDisks", SmartTraceCategory::ERROR_EXCEPTION,
                          "No disk found - reading S.M.A.R.T. requires administrator privileges");
        return;
    }

    for (DWORD i = 0; i < count; ++i) {
        // CrystalDiskInfo's first read happens before the vendor is known, so
        // its vendor-gated attribute parse (host reads/writes, life, wear
        // levelling) only lands on the refresh that follows.  Its own UI
        // calls UpdateSmartInfo() for every disk right after Init(); doing
        // the same here means the first paint already shows real numbers
        // instead of the -1 sentinels.
        m_cdi->UpdateSmartInfo(i);

        const cdi::DRIVE_INFO& asi = m_cdi->GetDisk(i);

        DiskEntry entry;
        entry.cdiIndex = i;
        entry.identity = smartcdi::MakeIdentity(asi, i);
        entry.smartAvailable = asi.IsSmartCorrect != FALSE;

        if (entry.identity.model.empty()) {
            entry.identity.model = L"PhysicalDrive" + std::to_wstring(asi.PhysicalDriveId);
        }

        char msg[384];
        snprintf(msg, sizeof(msg),
                 "Disk %u (PhysicalDrive%d): model=%s interface=%s cmd=%s smart=%s",
                 static_cast<unsigned>(i), asi.PhysicalDriveId,
                 WideToUtf8(entry.identity.model).c_str(),
                 entry.identity.interfaceName.c_str(),
                 WideToUtf8(asi.CommandTypeString).c_str(),
                 entry.smartAvailable ? "yes" : "no");
        SMART_TRACE_EVENT("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, msg);

        m_disks.push_back(entry);
    }

    char summary[128];
    snprintf(summary, sizeof(summary), "Enumeration complete: %u disks found",
             static_cast<unsigned>(count));
    SMART_TRACE_EVENT("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, summary);
}

// ── SMART data refresh ───────────────────────────────────────────────

void SmartMonitor::refreshSmartData() {
    if (m_selectedDiskIndex >= m_disks.size()) return;

    auto& entry = m_disks[m_selectedDiskIndex];
    SmartDataSnapshot newSnap;

    char msg[128];
    snprintf(msg, sizeof(msg), "Refreshing SMART data for disk %u", entry.cdiIndex);
    SMART_TRACE_BEGIN("refreshSmartData", SmartTraceCategory::DATA_REFRESH, msg);

    bool ok = readSmartDataForDisk(entry.cdiIndex, newSnap);
    newSnap.sampleTime = std::chrono::steady_clock::now();

    if (ok) {
        // Compute health
        computeHealth(newSnap);

        // Compute read/write rates
        computeRates(newSnap);

        // Update history
        {
            std::lock_guard<std::mutex> lk(m_dataMutex);

            // Temperature history (keep 1 hour = 60 minutes / refreshInterval)
            if (newSnap.temperatureCelsius > 0) {
                TempPoint tp;
                tp.timestamp = newSnap.sampleTime;
                tp.celsius = newSnap.temperatureCelsius;
                m_tempHistory.push_back(tp);

                // Keep last 1 hour max (but no more than 360 points)
                auto cutoff = newSnap.sampleTime - std::chrono::hours(1);
                while (m_tempHistory.size() > 360 ||
                       (!m_tempHistory.empty() && m_tempHistory.front().timestamp < cutoff)) {
                    m_tempHistory.pop_front();
                }

                // Track session max temperature
                if (newSnap.temperatureCelsius > newSnap.maxSessionTemp) {
                    newSnap.maxSessionTemp = newSnap.temperatureCelsius;
                }
            }

            // Health history (keep last 60 points for bar chart)
            m_healthHistory.push_back(newSnap.healthPercent);
            while (m_healthHistory.size() > 60) m_healthHistory.pop_front();

            // Store snapshot
            entry.snapshot = newSnap;
            entry.smartAvailable = newSnap.dataValid;
            entry.errorMessage = newSnap.errorMessage;
        }

        // Update overlay if active
        if (m_inOverlay.load()) {
            updateOverlayContent();
        }
    }

    // Trigger repaint
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    if (m_overlayHwnd) InvalidateRect(m_overlayHwnd, nullptr, FALSE);

    // Also refresh debug window
    if (m_debugWindowVisible && m_debugWindow) {
        m_debugWindow->refresh();
    }

    SMART_TRACE_END("refreshSmartData", SmartTraceCategory::DATA_REFRESH, ok ? "Success" : "Failed");
}

bool SmartMonitor::readSmartDataForDisk(uint32_t cdiIndex, SmartDataSnapshot& snapshot) {
    SMART_TRACE_BEGIN("readSmartDataForDisk", SmartTraceCategory::DATA_REFRESH,
                      "Refreshing via the CrystalDiskInfo port");

    if (!m_cdi || cdiIndex >= m_cdi->GetDiskCount()) {
        SMART_TRACE_END("readSmartDataForDisk", SmartTraceCategory::ERROR_EXCEPTION,
                        "Disk index out of range");
        snapshot.dataValid = false;
        snapshot.errorMessage = "Disk is no longer present.";
        return false;
    }

    // Re-read attributes and thresholds from the device.  The port also
    // recomputes its own overall-health verdict (CheckDiskStatus) as part of
    // this, which the adapter surfaces through smartReturnStatus.
    m_cdi->UpdateSmartInfo(cdiIndex);

    const cdi::DRIVE_INFO& asi = m_cdi->GetDisk(cdiIndex);

    // Drives whose S.M.A.R.T. read failed still have a usable identity, so
    // the snapshot is filled either way and dataValid carries the verdict.
    smartcdi::FillSnapshot(asi, cdiIndex, snapshot);

    if (!asi.IsSmartCorrect) {
        snapshot.dataValid = false;
        snapshot.errorMessage =
            "Failed to read S.M.A.R.T. attributes. Device may not support S.M.A.R.T. "
            "or requires admin privileges.";
        snapshot.permissionHint = L"Ensure the device supports S.M.A.R.T. and run as Administrator.";
        SMART_TRACE_END("readSmartDataForDisk", SmartTraceCategory::ERROR_EXCEPTION,
                        "S.M.A.R.T. data not reliable for this disk");
        return false;
    }

    // Session power-on hours (estimate from session start time)
    const auto now = std::chrono::steady_clock::now();
    snapshot.sessionPowerOnHours = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - m_sessionStart).count() / 3600);

    char msg[160];
    snprintf(msg, sizeof(msg), "Success: %zu attributes, temp=%.1fC, life=%lld%%",
             snapshot.attributes.size(), snapshot.temperatureCelsius,
             static_cast<long long>(snapshot.remainingLifePercent));
    SMART_TRACE_END("readSmartDataForDisk", SmartTraceCategory::DATA_REFRESH, msg);
    return true;
}

// ── Health computation (weighted algorithm) ──────────────────────────

void SmartMonitor::computeHealth(SmartDataSnapshot& snapshot) {
    SMART_TRACE_BEGIN("computeHealth", SmartTraceCategory::HEALTH_COMPUTE, "Starting health evaluation");

    if (!snapshot.dataValid) {
        snapshot.healthPercent = 0.0;
        snapshot.status = SmartStatus::Unknown;
        SMART_TRACE_END("computeHealth", SmartTraceCategory::HEALTH_COMPUTE, "Skipped: data invalid");
        return;
    }

    // Detect if this is an SSD (NVMe or has SSD-specific attributes)
    bool isSSD = (snapshot.identity.diskInterface == DiskInterfaceType::NVMe) ||
                 (snapshot.wearLevelingCount >= 0) ||
                 (snapshot.remainingLifePercent >= 0);

    // smartReturnStatus is the acquisition layer's own overall-health verdict
    // (CrystalDiskInfo's CheckDiskStatus, mapped by the adapter):
    //   0 = good, 1 = caution or bad, -1 = no verdict.
    //
    // For ATA drives a non-good verdict is definitive ("drive is failing").
    // For NVMe it is softer — CrystalDiskInfo raises CAUTION for conditions
    // that can be transient, a temperature excursion above all — so it caps
    // the weighted score instead of zeroing it.  Capping is necessary
    // because the weighted loop cannot see NVMe log fields at all: the NVMe
    // interpreter fills RawValue only, leaving current and threshold at 0,
    // so every NVMe attribute scores as healthy.
    bool nvmeVerdictNotGood = false;
    if (snapshot.smartReturnStatus == 1) {
        if (snapshot.identity.diskInterface != DiskInterfaceType::NVMe) {
            snapshot.healthPercent = 0.0;
            snapshot.status = SmartStatus::Failed;
            SMART_TRACE_END("computeHealth", SmartTraceCategory::HEALTH_COMPUTE,
                            "FAILED: S.M.A.R.T. overall-health verdict is not good");
            return;
        }
        nvmeVerdictNotGood = true;
    }

    double totalPenalty = 0.0;
    double maxPenalty = 0.0;

    for (auto& attr : snapshot.attributes) {
        double attrHealth = 1.0; // 1.0 = perfect health for this attribute
        double weight = 0.0;

        // Compute attribute health: (current - threshold) / (100 - threshold)
        // ATA spec: normalized values range 1-253, threshold is the failure point
        if (attr.current > 0 && attr.threshold > 0) {
            if (attr.current <= attr.threshold) {
                attrHealth = 0.0; // Below threshold = failed
            } else {
                attrHealth = static_cast<double>(attr.current - attr.threshold) /
                             static_cast<double>(std::max(1, 100 - attr.threshold));
                attrHealth = std::min(1.0, std::max(0.0, attrHealth));
            }
        }

        // Assign weights based on attribute ID
        switch (attr.id) {
        case 5:   // Reallocated Sector Count
            weight = SmartHealthWeights::REALLOCATED_SECTOR_COUNT;
            break;
        case 10:  // Spin Retry Count
            weight = SmartHealthWeights::SPIN_RETRY_COUNT;
            break;
        case 184: // End-to-End Error
            weight = SmartHealthWeights::END_TO_END_ERROR;
            break;
        case 187: // Reported Uncorrectable
            weight = SmartHealthWeights::REPORTED_UNCORRECTABLE;
            break;
        case 188: // Command Timeout
            weight = SmartHealthWeights::COMMAND_TIMEOUT;
            break;
        case 196: // Reallocated Event Count
            weight = SmartHealthWeights::REALLOCATED_EVENT_COUNT;
            break;
        case 197: // Current Pending Sector
            weight = SmartHealthWeights::CURRENT_PENDING_SECTOR;
            break;
        case 198: // Offline Uncorrectable
            weight = SmartHealthWeights::OFFLINE_UNCORRECTABLE;
            break;
        case 199: // UltraDMA CRC Error
            weight = SmartHealthWeights::ULTRA_DMA_CRC_ERROR;
            break;
        }

        // SSD-specific weights
        if (isSSD) {
            switch (attr.id) {
            case 177: // Wear Leveling Count
                weight = std::max(weight, SmartHealthWeights::WEAR_LEVELING_COUNT);
                break;
            case 202: // Percentage Used / Remaining Life
                weight = std::max(weight, SmartHealthWeights::SSD_REMAINING_LIFE);
                break;
            case 172: case 182: // Erase Fail Count
                weight = std::max(weight, SmartHealthWeights::ERASE_FAIL_COUNT);
                break;
            case 171: case 181: // Program Fail Count
                weight = std::max(weight, SmartHealthWeights::PROGRAM_FAIL_COUNT);
                break;
            case 174: case 192: // Unexpected Power Loss
                weight = std::max(weight, SmartHealthWeights::UNEXPECTED_POWER_LOSS);
                break;
            }
        }

        // NVMe-specific health factors.
        //
        // Keyed on CrystalDiskInfo's NVMe attribute ids rather than on names:
        // the acquisition layer names these from its own language table, so
        // the media-error entry is "Media and Data Integrity Errors" and a
        // name-based test for "Media Errors" never matched.  The ids are
        // stable — see the id table in src/CdiSmartNvmeInterp.cpp.
        if (snapshot.identity.diskInterface == DiskInterfaceType::NVMe) {
            switch (attr.id) {
            case 0x01: weight = std::max(weight, SmartHealthWeights::NVME_CRITICAL_WARNING); break;
            case 0x05: weight = std::max(weight, SmartHealthWeights::NVME_PERCENTAGE_USED); break;
            case 0x0D: weight = std::max(weight, SmartHealthWeights::NVME_UNSAFE_SHUTDOWNS); break;
            case 0x0E: weight = std::max(weight, SmartHealthWeights::NVME_MEDIA_ERRORS); break;
            default: break;
            }
        }

        maxPenalty += weight;
        totalPenalty += weight * (1.0 - attrHealth);
    }

    // Compute final health percentage
    if (maxPenalty > 0.0) {
        snapshot.healthPercent = (1.0 - (totalPenalty / maxPenalty)) * 100.0;
    } else {
        snapshot.healthPercent = 100.0;
    }

    snapshot.healthPercent = std::max(0.0, std::min(100.0, snapshot.healthPercent));

    // An NVMe verdict of caution or worse floors the score at "Warning": the
    // weighted score above is blind to the NVMe log fields, so without this
    // a drive with media errors or exhausted spare would still read 100%.
    if (nvmeVerdictNotGood) {
        snapshot.healthPercent = std::min(snapshot.healthPercent, HealthGrades::GOOD - 1.0);
    }

    // Determine status
    if (snapshot.healthPercent >= HealthGrades::EXCELLENT) {
        snapshot.status = SmartStatus::OK;
    } else if (snapshot.healthPercent >= HealthGrades::GOOD) {
        snapshot.status = SmartStatus::OK;
    } else if (snapshot.healthPercent >= HealthGrades::WARNING) {
        snapshot.status = SmartStatus::Warning;
    } else {
        snapshot.status = SmartStatus::Failed;
    }

    char msg[64];
    snprintf(msg, sizeof(msg), "Health=%.1f%% isSSD=%s penalty=%.3f/%0.3f",
             snapshot.healthPercent, isSSD ? "yes" : "no", totalPenalty, maxPenalty);
    SMART_TRACE_END("computeHealth", SmartTraceCategory::HEALTH_COMPUTE, msg);
}

// ── Rate computation ─────────────────────────────────────────────────

void SmartMonitor::computeRates(SmartDataSnapshot& snapshot) {
    if (m_prevSnapshot.dataValid && m_prevSampleTime.time_since_epoch().count() > 0) {
        double elapsed = std::chrono::duration<double>(
            snapshot.sampleTime - m_prevSampleTime).count();

        if (elapsed > 0.0) {
            uint64_t dRead = snapshot.totalBytesRead - m_prevSnapshot.totalBytesRead;
            uint64_t dWrite = snapshot.totalBytesWritten - m_prevSnapshot.totalBytesWritten;
            snapshot.readRateMBps = static_cast<double>(dRead) / elapsed / (1024.0 * 1024.0);
            snapshot.writeRateMBps = static_cast<double>(dWrite) / elapsed / (1024.0 * 1024.0);
        }
    }

    m_prevSnapshot = snapshot;
    m_prevSampleTime = snapshot.sampleTime;
}

// ── Refresh thread ───────────────────────────────────────────────────

void SmartMonitor::startRefreshThread() {
    SMART_TRACE_EVENT("startRefreshThread", SmartTraceCategory::THREAD_LIFECYCLE, "Creating refresh thread");
    m_running = true;
    m_hRefreshThread = CreateThread(nullptr, 0, refreshThreadProc, this, 0, nullptr);
}

void SmartMonitor::stopRefreshThread() {
    SMART_TRACE_EVENT("stopRefreshThread", SmartTraceCategory::THREAD_LIFECYCLE, "Stopping refresh thread");
    m_running = false;
    if (m_hRefreshThread) {
        WaitForSingleObject(m_hRefreshThread, 5000);
        CloseHandle(m_hRefreshThread);
        m_hRefreshThread = nullptr;
    }
}

DWORD WINAPI SmartMonitor::refreshThreadProc(LPVOID param) {
    auto* self = static_cast<SmartMonitor*>(param);
    SMART_TRACE_EVENT("refreshThreadProc", SmartTraceCategory::THREAD_LIFECYCLE, "Refresh thread started");

    while (self->m_running.load()) {
        // Wait for the refresh interval, checking every second for changes
        int interval = self->m_refreshIntervalSec.load();
        if (self->m_backgroundMode.load()) {
            interval *= 2; // Double interval when in background
        }

        for (int i = 0; i < interval && self->m_running.load(); ++i) {
            // Check if immediate refresh was requested
            if (self->m_needsRefresh.exchange(false)) {
                break;
            }
            Sleep(1000);
        }

        if (!self->m_running.load()) break;

        self->refreshSmartData();
    }

    SMART_TRACE_EVENT("refreshThreadProc", SmartTraceCategory::THREAD_LIFECYCLE, "Refresh thread exiting");
    return 0;
}

void SmartMonitor::onRefreshNow() {
    m_needsRefresh = true;
}

// ── Overlay toggle ───────────────────────────────────────────────────

void SmartMonitor::toggleOverlay() {
    if (m_inOverlay.load()) {
        // Close overlay
        if (m_overlayHwnd) {
            DestroyWindow(m_overlayHwnd);
            m_overlayHwnd = nullptr;
        }
        m_inOverlay = false;
        ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
    } else {
        // Open overlay
        if (createOverlayWindow()) {
            m_inOverlay = true;
            updateOverlayContent();
            ShowWindow(m_hwnd, SW_MINIMIZE);
        }
    }
}

void SmartMonitor::updateOverlayContent() {
    if (!m_overlayHwnd) return;
    InvalidateRect(m_overlayHwnd, nullptr, FALSE);
    UpdateWindow(m_overlayHwnd);
}

// ── Formatting helpers ───────────────────────────────────────────────

std::wstring SmartMonitor::fmtBytesSmart(uint64_t bytes) const {
    if (bytes == 0) return L"0 B";
    const wchar_t* units[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0;
    double v = static_cast<double>(bytes);
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    wchar_t buf[64];
    if (v >= 100.0) swprintf(buf, 64, L"%.0f %ls", v, units[i]);
    else if (v >= 10.0) swprintf(buf, 64, L"%.1f %ls", v, units[i]);
    else swprintf(buf, 64, L"%.2f %ls", v, units[i]);
    return buf;
}

std::wstring SmartMonitor::fmtRateMBps(double mbps) const {
    wchar_t buf[32];
    if (mbps < 0.01) swprintf(buf, 32, L"0.00 MB/s");
    else if (mbps >= 100.0) swprintf(buf, 32, L"%.0f MB/s", mbps);
    else swprintf(buf, 32, L"%.2f MB/s", mbps);
    return buf;
}

std::wstring SmartMonitor::fmtHours(uint64_t hours) const {
    wchar_t buf[64];
    if (hours >= 8760) { // >= 1 year
        double years = hours / 8760.0;
        swprintf(buf, 64, L"%.1f y (%llu h)", years, hours);
    } else if (hours >= 24) {
        uint64_t days = hours / 24;
        swprintf(buf, 64, L"%llu d %llu h", days, hours % 24);
    } else {
        swprintf(buf, 64, L"%llu h", hours);
    }
    return buf;
}

std::wstring SmartMonitor::fmtTemperature(double celsius) const {
    wchar_t buf[32];
    swprintf(buf, 32, L"%.1f °C", celsius);
    return buf;
}

COLORREF SmartMonitor::healthColor(double percent) const {
    if (percent >= HealthGrades::EXCELLENT) return SmartColors::HEALTH_GREEN;
    if (percent >= HealthGrades::GOOD) return SmartColors::HEALTH_YELLOW;
    if (percent >= HealthGrades::WARNING) return SmartColors::HEALTH_ORANGE;
    return SmartColors::HEALTH_RED;
}

COLORREF SmartMonitor::tempColor(double celsius) const {
    if (celsius < 35.0) return SmartColors::TEMP_COLD;
    if (celsius < m_highTempThreshold) return SmartColors::TEMP_WARM;
    return SmartColors::TEMP_HOT;
}

// =====================================================================
//  WINDOW PROCEDURES
// =====================================================================

// ── Main window procedure ────────────────────────────────────────────

LRESULT CALLBACK SmartMonitor::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SmartMonitor* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SmartMonitor*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SmartMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_SIZE:
        if (self) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (self) self->paint(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEWHEEL: {
        // The attribute table is the only scrollable region on the page: a
        // drive with many attributes lists more rows than the band holds.
        // m_attrBandTop is published by paint() and stays 0 until the first
        // one, so a wheel event above the table falls through untouched.
        if (!self || self->m_attrBandTop <= 0) break;

        POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        if (pt.y < self->m_attrBandTop) break;

        const int rows = (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? -3 : 3;

        size_t attrCount = 0;
        {
            std::lock_guard<std::mutex> lk(self->m_dataMutex);
            if (self->m_selectedDiskIndex < self->m_disks.size()) {
                attrCount = self->m_disks[self->m_selectedDiskIndex].snapshot.attributes.size();
            }
        }

        const int maxScroll = std::max(0, static_cast<int>(attrCount) - self->m_attrRowsFit);
        const int next = std::min(maxScroll, std::max(0, self->m_attrScroll + rows));
        if (next != self->m_attrScroll) {
            self->m_attrScroll = next;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        if (!self) break;
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);

        // Disk selector clicks (row at y=60-100 area)
        RECT rc;
        GetClientRect(hwnd, &rc);
        int w = rc.right;

        if (y >= 55 && y <= 95) {
            // Disk selector row
            size_t count = self->m_disks.size();
            int diskW = std::min(180, (w - 40) / std::max(static_cast<int>(count), 1));
            for (size_t i = 0; i < count; ++i) {
                int dx = 15 + static_cast<int>(i) * (diskW + 8);
                if (x >= dx && x <= dx + diskW) {
                    self->m_selectedDiskIndex = i;
                    self->m_attrScroll = 0;   // new drive, new attribute list
                    self->m_prevSnapshot = SmartDataSnapshot{}; // Reset rate calc
                    self->onRefreshNow();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    break;
                }
            }
        }

        // Refresh button (at right side of toolbar)
        if (y >= 5 && y <= 45 && x >= w - 200 && x <= w - 20) {
            self->onRefreshNow();
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        // Overlay toggle button
        if (y >= 5 && y <= 45 && x >= w - 340 && x <= w - 210) {
            self->toggleOverlay();
        }

        return 0;
    }

    case WM_KEYDOWN: {
        if (!self) break;
        switch (wp) {
        case VK_F5:
            SMART_TRACE_EVENT("wndProc", SmartTraceCategory::USER_INTERACTION, "F5 pressed: refresh now");
            self->onRefreshNow();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case 'O':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                SMART_TRACE_EVENT("wndProc", SmartTraceCategory::USER_INTERACTION, "Ctrl+O: toggle overlay");
                self->toggleOverlay();
                return 0;
            }
            break;
        case VK_ESCAPE:
            if (self->m_inOverlay.load()) {
                SMART_TRACE_EVENT("wndProc", SmartTraceCategory::USER_INTERACTION, "ESC: close overlay");
                self->toggleOverlay();
                return 0;
            }
            break;
        case 'D':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                SMART_TRACE_EVENT("wndProc", SmartTraceCategory::USER_INTERACTION, "Ctrl+D: toggle debug window");
                self->toggleDebugWindow();
                return 0;
            }
            break;
        }
        break;
    }

    case WM_CLOSE:
        if (self) {
            self->m_running = false;
            self->stopRefreshThread();
            if (self->m_overlayHwnd) {
                DestroyWindow(self->m_overlayHwnd);
                self->m_overlayHwnd = nullptr;
            }
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    case WM_ACTIVATE:
        if (self) {
            if (LOWORD(wp) == WA_INACTIVE) {
                self->m_backgroundMode = true;
            } else {
                self->m_backgroundMode = false;
                // Coming back to foreground: do an immediate refresh
                self->onRefreshNow();
            }
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ── Overlay window procedure ─────────────────────────────────────────

LRESULT CALLBACK SmartMonitor::overlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    SmartMonitor* self = nullptr;

    if (msg == WM_CREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<SmartMonitor*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SmartMonitor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_CREATE:
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (self) self->paintOverlay(hdc, ps.rcPaint);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN: {
        if (self) {
            int y = GET_Y_LPARAM(lp);
            if (y < 28) {
                // Title bar drag
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
        }
        break;
    }

    case WM_LBUTTONDBLCLK:
        // Double-click to restore main window
        if (self) {
            self->toggleOverlay();
        }
        return 0;

    case WM_RBUTTONUP: {
        if (self) {
            POINT pt; GetCursorPos(&pt);
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Restore Full Window");
            AppendMenuW(menu, MF_STRING, 2, L"Refresh Now");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 3, L"Close SMART Monitor");
            SetForegroundWindow(hwnd);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY,
                                     pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == 1) self->toggleOverlay();
            else if (cmd == 2) { self->onRefreshNow(); InvalidateRect(hwnd, nullptr, FALSE); }
            else if (cmd == 3) PostMessageW(self->m_hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
        if (self) self->toggleOverlay();
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wp, lp);
}

// =====================================================================
//  PAINT: MAIN WINDOW
// =====================================================================

void SmartMonitor::paint(HDC hdc, const RECT& rc) {
    RECT client;
    GetClientRect(m_hwnd, &client);
    int w = client.right;
    int h = client.bottom;

    // Double buffering
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(SmartColors::BG_DARK);
    FillRect(memDC, &client, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(memDC, TRANSPARENT);

    // ── Title bar ──
    {
        RECT titleRc = {0, 0, w, 48};
        HBRUSH titleBg = CreateSolidBrush(SmartColors::BG_HEADER);
        FillRect(memDC, &titleRc, titleBg);
        DeleteObject(titleBg);

        HPEN titleBorder = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        HPEN oldPen = (HPEN)SelectObject(memDC, titleBorder);
        MoveToEx(memDC, 0, 48, nullptr);
        LineTo(memDC, w, 48);
        SelectObject(memDC, oldPen);
        DeleteObject(titleBorder);

        SelectObject(memDC, m_hFontTitle);
        SetTextColor(memDC, SmartColors::ACCENT_CYAN);
        RECT titleText = {16, 4, 300, 44};
        DrawTextW(memDC, L"S.M.A.R.T. Monitor", -1, &titleText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Buttons on right side
        SelectObject(memDC, m_hFontSmall);

        // Refresh button
        {
            RECT btnRc = {w - 190, 8, w - 20, 40};
            HPEN btnPen = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HBRUSH btnBr = CreateSolidBrush(RGB(40, 42, 48));
            HPEN oldP = (HPEN)SelectObject(memDC, btnPen);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, btnBr);
            RoundRect(memDC, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom, 6, 6);
            SelectObject(memDC, oldP);
            SelectObject(memDC, oldB);
            DeleteObject(btnPen);
            DeleteObject(btnBr);

            SetTextColor(memDC, SmartColors::TEXT_PRIMARY);
            wchar_t btnText[64];
            swprintf(btnText, 64, L"Refresh Now  [%ds]", m_refreshIntervalSec.load());
            DrawTextW(memDC, btnText, -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Overlay button
        {
            RECT btnRc = {w - 340, 8, w - 200, 40};
            HPEN btnPen = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HBRUSH btnBr = CreateSolidBrush(RGB(40, 42, 48));
            HPEN oldP = (HPEN)SelectObject(memDC, btnPen);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, btnBr);
            RoundRect(memDC, btnRc.left, btnRc.top, btnRc.right, btnRc.bottom, 6, 6);
            SelectObject(memDC, oldP);
            SelectObject(memDC, oldB);
            DeleteObject(btnPen);
            DeleteObject(btnBr);

            SetTextColor(memDC, SmartColors::TEXT_PRIMARY);
            DrawTextW(memDC, L"Mini Overlay", -1, &btnRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // ── Disk selector ──
    int y = 55;
    drawDiskSelector(memDC, y, w);

    y += 5;

    // ── Vertical split ──────────────────────────────────────────
    // The attribute table is the densest element on the page and the only
    // one with six columns.  Those do not fit the 340 px right column — a
    // single row is roughly 500 px at the mono font, which silently clipped
    // the Raw column away — so the table gets its own full-width band along
    // the bottom, and the cards and charts above share what is left.
    y += 5;
    const int contentH = h - y - 40;
    const int bandTop  = y + contentH * 52 / 100;
    const int upperH   = bandTop - y - 10;
    m_attrBandTop = bandTop;

    // Get current snapshot
    SmartDataSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (m_selectedDiskIndex < m_disks.size()) {
            snap = m_disks[m_selectedDiskIndex].snapshot;
        }
    }

    // ── Layout: Left panel (overview) + Center (metric cards) + Right (charts) ──
    int leftW = 240;
    int rightW = 340;
    int centerX = leftW + 15;
    int centerW = w - leftW - rightW - 30;

    // Left: Disk overview
    {
        // Stops where the attribute band starts; the six info rows it holds
        // need far less height than the full column.
        RECT leftRc = {10, y, leftW, bandTop - 8};
        HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
        FillRect(memDC, &leftRc, cardBg);
        DeleteObject(cardBg);

        HPEN borderPen = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        HPEN oldP = (HPEN)SelectObject(memDC, borderPen);
        HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
        Rectangle(memDC, leftRc.left, leftRc.top, leftRc.right, leftRc.bottom);
        SelectObject(memDC, oldP);
        SelectObject(memDC, oldB);
        DeleteObject(borderPen);

        int ly = leftRc.top + 12;

        SelectObject(memDC, m_hFontTitle);
        SetTextColor(memDC, SmartColors::ACCENT_CYAN);
        RECT titleRc = {leftRc.left + 12, ly, leftRc.right - 12, ly + 24};
        DrawTextW(memDC, L"Disk Info", -1, &titleRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        ly += 30;

        // Separator
        HPEN sepPen = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        SelectObject(memDC, sepPen);
        MoveToEx(memDC, leftRc.left + 8, ly, nullptr);
        LineTo(memDC, leftRc.right - 8, ly);
        DeleteObject(sepPen);
        ly += 10;

        SelectObject(memDC, m_hFontSmall);
        auto drawInfoRow = [&](const wchar_t* label, const std::wstring& value, bool dim = false) {
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT lRc = {leftRc.left + 12, ly, leftRc.left + 80, ly + 20};
            DrawTextW(memDC, label, -1, &lRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SetTextColor(memDC, dim ? SmartColors::TEXT_DIM : SmartColors::TEXT_PRIMARY);
            RECT vRc = {leftRc.left + 84, ly, leftRc.right - 12, ly + 20};
            DrawTextW(memDC, value.c_str(), -1, &vRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ly += 22;
        };

        drawInfoRow(L"Model:", snap.identity.model);
        drawInfoRow(L"Firmware:", snap.identity.firmwareRevision);
        drawInfoRow(L"Serial:", snap.identity.serialNumber);
        drawInfoRow(L"Interface:", snap.identity.interfaceName.empty()
                     ? L"Unknown" : ataToWide(snap.identity.interfaceName));
        drawInfoRow(L"Capacity:", fmtBytesSmart(snap.identity.capacityBytes));
        drawInfoRow(L"Sector:", std::to_wstring(snap.identity.sectorSize) + L" B");

        ly += 10;
        HPEN sepPen2 = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        SelectObject(memDC, sepPen2);
        MoveToEx(memDC, leftRc.left + 8, ly, nullptr);
        LineTo(memDC, leftRc.right - 8, ly);
        DeleteObject(sepPen2);
        ly += 10;

        // SMART status
        SelectObject(memDC, m_hFontSmall);
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT stLabel = {leftRc.left + 12, ly, leftRc.left + 80, ly + 20};
        DrawTextW(memDC, L"SMART:", -1, &stLabel, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        if (snap.dataValid) {
            SetTextColor(memDC, SmartColors::HEALTH_GREEN);
            RECT stVal = {leftRc.left + 84, ly, leftRc.right - 12, ly + 20};
            DrawTextW(memDC, L"OK", -1, &stVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            SetTextColor(memDC, SmartColors::HEALTH_RED);
            RECT stVal = {leftRc.left + 84, ly, leftRc.right - 12, ly + 20};
            DrawTextW(memDC, L"Not Available", -1, &stVal, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }


    // Center: Metric cards
    {
        int cardW = (centerW - 20) / 2;
        int cardH = (upperH - 20) / 2;

        // Card 1: Temperature
        {
            int cx = centerX;
            int cy = y;
            RECT cardRc = {cx, cy, cx + cardW, cy + cardH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &cardRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 2,
                (snap.temperatureCelsius >= m_highTempThreshold) ? SmartColors::HEALTH_RED : SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            // Title
            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {cx + 12, cy + 8, cx + cardW - 12, cy + 30};
            DrawTextW(memDC, L"Temperature", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Value
            SelectObject(memDC, m_hFontTitle);
            SetTextColor(memDC, tempColor(snap.temperatureCelsius));
            RECT vRc = {cx + 12, cy + 30, cx + cardW - 12, cy + 60};
            DrawTextW(memDC, fmtTemperature(snap.temperatureCelsius).c_str(), -1, &vRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Session max
            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT mRc = {cx + 12, cy + 60, cx + cardW - 12, cy + 78};
            wchar_t mxBuf[64];
            swprintf(mxBuf, 64, L"Session Max: %.1f °C", snap.maxSessionTemp);
            DrawTextW(memDC, mxBuf, -1, &mRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Mini temperature trend
            if (!m_tempHistory.empty()) {
                std::vector<TempPoint> temps(m_tempHistory.begin(), m_tempHistory.end());
                drawMiniLineChart(memDC, cx + 12, cy + 82, cardW - 24, cardH - 94,
                                  temps, 85.0, SmartColors::CHART_LINE);
            }
        }

        // Card 2: Health
        {
            int cx = centerX + cardW + 8;
            int cy = y;
            RECT cardRc = {cx, cy, cx + cardW, cy + cardH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &cardRc, cardBg);
            DeleteObject(cardBg);

            COLORREF hc = healthColor(snap.healthPercent);
            HPEN borderP = CreatePen(PS_SOLID, 2,
                (snap.healthPercent < HealthGrades::WARNING) ? SmartColors::HEALTH_RED : SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {cx + 12, cy + 8, cx + cardW - 12, cy + 30};
            DrawTextW(memDC, L"Health", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Ring gauge
            drawRingGauge(memDC, cx + cardW / 2, cy + 50 + (cardH - 50) / 2,
                          std::min(cardW, cardH) / 2 - 30,
                          snap.healthPercent, L"");

            // Percentage text
            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, hc);
            RECT pRc = {cx + 12, cy + cardH - 40, cx + cardW - 12, cy + cardH - 12};
            wchar_t pBuf[32];
            swprintf(pBuf, 32, L"%.1f%%", snap.healthPercent);
            DrawTextW(memDC, pBuf, -1, &pRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        // Card 3: Power-On Hours
        {
            int cx = centerX;
            int cy = y + cardH + 8;
            RECT cardRc = {cx, cy, cx + cardW, cy + cardH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &cardRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {cx + 12, cy + 8, cx + cardW - 12, cy + 30};
            DrawTextW(memDC, L"Power-On Time", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Total
            SelectObject(memDC, m_hFontTitle);
            SetTextColor(memDC, SmartColors::TEXT_PRIMARY);
            RECT vRc = {cx + 12, cy + 30, cx + cardW - 12, cy + 60};
            DrawTextW(memDC, fmtHours(snap.powerOnHours).c_str(), -1, &vRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT sRc = {cx + 12, cy + 60, cx + cardW - 12, cy + 78};
            wchar_t sessBuf[64];
            swprintf(sessBuf, 64, L"This session: %llu h", snap.sessionPowerOnHours);
            DrawTextW(memDC, sessBuf, -1, &sRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        // Card 4: Read/Write Totals
        {
            int cx = centerX + cardW + 8;
            int cy = y + cardH + 8;
            RECT cardRc = {cx, cy, cx + cardW, cy + cardH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &cardRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, cardRc.left, cardRc.top, cardRc.right, cardRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {cx + 12, cy + 8, cx + cardW - 12, cy + 30};
            DrawTextW(memDC, L"Total I/O", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Read
            SetTextColor(memDC, SmartColors::READ_COLOR);
            SelectObject(memDC, m_hFontBody);
            RECT rRc = {cx + 12, cy + 32, cx + cardW - 12, cy + 54};
            wchar_t rBuf[64];
            swprintf(rBuf, 64, L"R: %ls", fmtBytesSmart(snap.totalBytesRead).c_str());
            DrawTextW(memDC, rBuf, -1, &rRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Write
            SetTextColor(memDC, SmartColors::WRITE_COLOR);
            RECT wRc = {cx + 12, cy + 56, cx + cardW - 12, cy + 78};
            wchar_t wBuf[64];
            swprintf(wBuf, 64, L"W: %ls", fmtBytesSmart(snap.totalBytesWritten).c_str());
            DrawTextW(memDC, wBuf, -1, &wRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Rates
            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT rateRc = {cx + 12, cy + 78, cx + cardW - 12, cy + 96};
            wchar_t rateBuf[64];
            swprintf(rateBuf, 64, L"Rate: R %ls  W %ls",
                     fmtRateMBps(snap.readRateMBps).c_str(),
                     fmtRateMBps(snap.writeRateMBps).c_str());
            DrawTextW(memDC, rateBuf, -1, &rateRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Right: Charts (the attribute table now lives in the band below)
    {
        int rx = w - rightW - 5;
        int ry = y;

        const int healthChartH = std::min(160, std::max(90, upperH * 55 / 100));

        // Health history bar chart
        {
            RECT chartRc = {rx, ry, rx + rightW, ry + healthChartH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &chartRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, chartRc.left, chartRc.top, chartRc.right, chartRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {rx + 12, ry + 6, rx + rightW - 12, ry + 24};
            DrawTextW(memDC, L"Health History", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Health history bar chart
            std::vector<double> hhist(m_healthHistory.begin(), m_healthHistory.end());
            drawMiniBarChart(memDC, rx + 16, ry + 28, rightW - 32,
                             std::max(24, healthChartH - 40), hhist, SmartColors::HEALTH_GREEN);
        }

        ry += healthChartH + 8;

        // Read/Write rate sparkline
        {
            const int rateCardH = std::max(90, std::min(120, bandTop - ry - 8));
            RECT chartRc = {rx, ry, rx + rightW, ry + rateCardH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &chartRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, chartRc.left, chartRc.top, chartRc.right, chartRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {rx + 12, ry + 6, rx + rightW - 12, ry + 24};
            DrawTextW(memDC, L"Current Rates", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Show read rate and write rate as text
            SelectObject(memDC, m_hFontBody);
            SetTextColor(memDC, SmartColors::READ_COLOR);
            RECT rrRc = {rx + 16, ry + 30, rx + rightW - 16, ry + 52};
            wchar_t rrBuf[64];
            swprintf(rrBuf, 64, L"Read:  %ls", fmtRateMBps(snap.readRateMBps).c_str());
            DrawTextW(memDC, rrBuf, -1, &rrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SetTextColor(memDC, SmartColors::WRITE_COLOR);
            RECT wrRc = {rx + 16, ry + 56, rx + rightW - 16, ry + 78};
            wchar_t wrBuf[64];
            swprintf(wrBuf, 64, L"Write: %ls", fmtRateMBps(snap.writeRateMBps).c_str());
            DrawTextW(memDC, wrBuf, -1, &wrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

    }

    // ── S.M.A.R.T. attribute table (full width) ──────────────────────
    //
    // Laid out column by column rather than as one space-formatted string.
    // The old single-line format needed roughly 500 px, so inside the 340 px
    // right column everything from the Raw column onwards was clipped away,
    // and the hard-coded header never lined up with the rows.  Each cell now
    // gets its own rect, so columns align exactly and over-long values are
    // ellipsised instead of silently disappearing.
    if (!snap.attributes.empty()) {
        const int bandLeft   = 10;
        const int bandRight  = w - 5;
        const int bandBottom = h - 44;      // leave room for the footer

        RECT bandRc = {bandLeft, bandTop, bandRight, bandBottom};
        HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
        FillRect(memDC, &bandRc, cardBg);
        DeleteObject(cardBg);

        HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        HPEN oldP = (HPEN)SelectObject(memDC, borderP);
        HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
        Rectangle(memDC, bandRc.left, bandRc.top, bandRc.right, bandRc.bottom);
        SelectObject(memDC, oldP); SelectObject(memDC, oldB);
        DeleteObject(borderP);

        // ── Column geometry ──
        const int pad = 12;
        const int gap = 18;                 // the band is wide; let it breathe
        const int idW = 34, valueW = 64, worstW = 64, threshW = 64;
        const int inner = (bandRight - pad) - (bandLeft + pad);
        // Name and Raw share everything the fixed columns leave, and Raw takes
        // the remainder so the last column always ends inside the band.
        const int avail = inner - (idW + valueW + worstW + threshW + gap * 5);
        const int nameW = std::max(120, avail * 38 / 100);
        const int rawW  = std::max(120, avail - nameW);

        int colX[6] = {0};
        colX[0] = bandLeft + pad;
        colX[1] = colX[0] + idW + gap;
        colX[2] = colX[1] + nameW + gap;
        colX[3] = colX[2] + valueW + gap;
        colX[4] = colX[3] + worstW + gap;
        colX[5] = colX[4] + threshW + gap;
        const int colW[6] = {idW, nameW, valueW, worstW, threshW, rawW};

        // ── Title ──
        SelectObject(memDC, m_hFontSmall);
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT titleRc = {bandLeft + pad, bandTop + 6, bandRight - pad, bandTop + 24};
        DrawTextW(memDC, L"SMART Attributes", -1, &titleRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // ── Header ──
        int ay = bandTop + 28;
        SetTextColor(memDC, SmartColors::TEXT_DIM);
        const wchar_t* const headerText[6] = {
            L"ID", L"Attribute Name", L"Value", L"Worst", L"Thresh", L"Raw"
        };
        for (int c = 0; c < 6; ++c) {
            RECT r = {colX[c], ay, colX[c] + colW[c], ay + 16};
            const UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                             ((c >= 2 && c <= 4) ? DT_RIGHT : DT_LEFT);
            DrawTextW(memDC, headerText[c], -1, &r, fmt);
        }
        ay += 18;

        HPEN sepP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        SelectObject(memDC, sepP);
        MoveToEx(memDC, bandLeft + 8, ay, nullptr);
        LineTo(memDC, bandRight - 8, ay);
        DeleteObject(sepP);

        // ── Rows ──
        SelectObject(memDC, m_hFontMono);
        const int rowH = 18;
        const int firstRowY = ay + 4;
        const int availRows = std::max(0, (bandBottom - 6 - firstRowY) / rowH);
        m_attrRowsFit = availRows;

        const int total = static_cast<int>(snap.attributes.size());
        const int maxScroll = std::max(0, total - availRows);
        // Also clamped here, not just in the wheel handler: the table is
        // shorter on a drive with fewer attributes.
        const int first = std::min(m_attrScroll, maxScroll);
        const int last = std::min(total, first + availRows);

        for (int i = first; i < last; ++i) {
            const SmartAttribute& attr = snap.attributes[i];
            const int rowY = firstRowY + (i - first) * rowH;

            if (((i - first) & 1) != 0) {   // zebra striping
                RECT zr = {bandLeft + 8, rowY, bandRight - 8, rowY + rowH};
                HBRUSH zb = CreateSolidBrush(RGB(24, 24, 28));
                FillRect(memDC, &zr, zb);
                DeleteObject(zb);
            }

            // Colour carries state, not category: a pre-failure attribute is
            // highlighted only once it actually reaches its threshold.  The
            // old rule also tinted every pre-failure row yellow regardless of
            // its value, which marked healthy drives and told the reader
            // nothing.
            COLORREF textColor = SmartColors::TEXT_PRIMARY;
            if (attr.preFailure && attr.current <= attr.threshold) {
                textColor = SmartColors::HEALTH_RED;
            }

            wchar_t buf[32];
            swprintf(buf, 32, L"%02X", attr.id);
            RECT idRc = {colX[0], rowY, colX[0] + colW[0], rowY + rowH};
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            DrawTextW(memDC, buf, -1, &idRc, DT_VCENTER | DT_SINGLELINE);

            SetTextColor(memDC, textColor);
            RECT nameRc = {colX[1], rowY, colX[1] + colW[1], rowY + rowH};
            const std::wstring name = ataToWide(attr.name);
            DrawTextW(memDC, name.c_str(), -1, &nameRc,
                      DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            const unsigned norm[3] = {attr.current, attr.worst, attr.threshold};
            for (int c = 0; c < 3; ++c) {
                wchar_t nb[16];
                swprintf(nb, 16, L"%u", norm[c]);
                RECT nr = {colX[2 + c], rowY, colX[2 + c] + colW[2 + c], rowY + rowH};
                DrawTextW(memDC, nb, -1, &nr, DT_VCENTER | DT_SINGLELINE | DT_RIGHT);
            }

            SetTextColor(memDC, SmartColors::ACCENT_CYAN);
            RECT rawRc = {colX[5], rowY, colX[5] + colW[5], rowY + rowH};
            const std::wstring raw = ataToWide(attr.rawString);
            DrawTextW(memDC, raw.c_str(), -1, &rawRc,
                      DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // Scroll hint, only when the list actually overflows.
        if (total > availRows) {
            wchar_t hint[128];
            swprintf(hint, 128, L"showing %d-%d of %d  -  wheel scrolls",
                     first + 1, last, total);
            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT hintRc = {bandRight - 560, bandTop + 6, bandRight - pad - 8, bandTop + 24};
            DrawTextW(memDC, hint, -1, &hintRc,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Footer
    {
        RECT footRc = {0, h - 36, w, h};
        HBRUSH footBg = CreateSolidBrush(SmartColors::BG_HEADER);
        FillRect(memDC, &footRc, footBg);
        DeleteObject(footBg);

        HPEN footBorder = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
        HPEN oldP = (HPEN)SelectObject(memDC, footBorder);
        MoveToEx(memDC, 0, h - 36, nullptr);
        LineTo(memDC, w, h - 36);
        SelectObject(memDC, oldP);
        DeleteObject(footBorder);

        SelectObject(memDC, m_hFontSmall);
        SetTextColor(memDC, SmartColors::TEXT_DIM);
        RECT fRc = {12, h - 32, w - 12, h - 4};
        wchar_t fBuf[128];
        swprintf(fBuf, 128, L"Refresh: %ds | F5=Refresh | Ctrl+O=Overlay | ESC=Close Overlay | Click disk tab to switch",
                 m_refreshIntervalSec.load());
        DrawTextW(memDC, fBuf, -1, &fRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    // Blit
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

// =====================================================================
//  PAINT: OVERLAY WINDOW
// =====================================================================

void SmartMonitor::paintOverlay(HDC hdc, const RECT& rc) {
    RECT client;
    GetClientRect(m_overlayHwnd, &client);
    int w = client.right;
    int h = client.bottom;

    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, memBmp);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(RGB(20, 20, 24));
    FillRect(memDC, &client, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(memDC, TRANSPARENT);

    SmartDataSnapshot snap;
    {
        std::lock_guard<std::mutex> lk(m_dataMutex);
        if (m_selectedDiskIndex < m_disks.size()) {
            snap = m_disks[m_selectedDiskIndex].snapshot;
        }
    }

    // Determine if alert state
    bool alert = (snap.healthPercent < HealthGrades::WARNING) ||
                 (snap.temperatureCelsius >= m_highTempThreshold);

    // Alert border
    COLORREF borderColor = alert ? SmartColors::HEALTH_RED : SmartColors::BORDER;
    HPEN borderPen = CreatePen(PS_SOLID, alert ? 3 : 1, borderColor);
    HPEN oldPen = (HPEN)SelectObject(memDC, borderPen);
    HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBr = (HBRUSH)SelectObject(memDC, nullBr);
    Rectangle(memDC, 0, 0, w, h);
    SelectObject(memDC, oldPen);
    SelectObject(memDC, oldBr);
    DeleteObject(borderPen);

    // Title bar
    HBRUSH titleBg = CreateSolidBrush(RGB(28, 28, 34));
    RECT titleRc = {1, 1, w - 1, 28};
    FillRect(memDC, &titleRc, titleBg);
    DeleteObject(titleBg);

    SelectObject(memDC, m_hFontOverlay);
    SetTextColor(memDC, SmartColors::ACCENT_CYAN);
    RECT tRc = {8, 2, w - 30, 26};
    DrawTextW(memDC, L"SMART Monitor", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // Alert indicator
    if (alert) {
        SetTextColor(memDC, SmartColors::HEALTH_RED);
        RECT aRc = {w - 30, 2, w - 6, 26};
        DrawTextW(memDC, L"!", -1, &aRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    int y = 34;

    // Temperature
    {
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT lRc = {8, y, 70, y + 22};
        DrawTextW(memDC, L"Temp:", -1, &lRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, tempColor(snap.temperatureCelsius));
        RECT vRc = {72, y, w - 12, y + 22};
        DrawTextW(memDC, fmtTemperature(snap.temperatureCelsius).c_str(), -1, &vRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 28;
    }

    // Health
    {
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT lRc = {8, y, 70, y + 22};
        DrawTextW(memDC, L"Health:", -1, &lRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        COLORREF hc = healthColor(snap.healthPercent);
        SetTextColor(memDC, hc);
        RECT vRc = {72, y, w - 12, y + 22};
        wchar_t buf[32];
        swprintf(buf, 32, L"%.1f%%", snap.healthPercent);
        DrawTextW(memDC, buf, -1, &vRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Mini health progress bar
        int barX = 72;
        int barW = w - 92;
        int barY = y + 24;
        int barH = 6;

        HBRUSH barBg = CreateSolidBrush(RGB(40, 40, 46));
        RECT barRc = {barX, barY, barX + barW, barY + barH};
        FillRect(memDC, &barRc, barBg);
        DeleteObject(barBg);

        int fillW = static_cast<int>(barW * snap.healthPercent / 100.0);
        if (fillW > 0) {
            HBRUSH fillBr = CreateSolidBrush(hc);
            RECT fillRc = {barX, barY, barX + fillW, barY + barH};
            FillRect(memDC, &fillRc, fillBr);
            DeleteObject(fillBr);
        }
        y += 38;
    }

    // Read/Write totals
    {
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT lRc = {8, y, 70, y + 22};
        DrawTextW(memDC, L"Total R:", -1, &lRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, SmartColors::READ_COLOR);
        RECT vRc = {72, y, w - 12, y + 22};
        DrawTextW(memDC, fmtBytesSmart(snap.totalBytesRead).c_str(), -1, &vRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 24;

        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT l2Rc = {8, y, 70, y + 22};
        DrawTextW(memDC, L"Total W:", -1, &l2Rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, SmartColors::WRITE_COLOR);
        RECT v2Rc = {72, y, w - 12, y + 22};
        DrawTextW(memDC, fmtBytesSmart(snap.totalBytesWritten).c_str(), -1, &v2Rc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 30;
    }

    // Power-On Hours
    {
        SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
        RECT lRc = {8, y, 70, y + 22};
        DrawTextW(memDC, L"POH:", -1, &lRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        SetTextColor(memDC, SmartColors::TEXT_PRIMARY);
        RECT vRc = {72, y, w - 12, y + 22};
        DrawTextW(memDC, fmtHours(snap.powerOnHours).c_str(), -1, &vRc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 30;
    }

    // Footer hint
    SelectObject(memDC, m_hFontSmall);
    SetTextColor(memDC, SmartColors::TEXT_DIM);
    RECT hintRc = {8, h - 24, w - 8, h - 4};
    DrawTextW(memDC, L"Double-click to restore", -1, &hintRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);
}

// =====================================================================
//  DRAWING HELPERS
// =====================================================================

void SmartMonitor::drawDiskSelector(HDC hdc, int& y, int w) {
    SelectObject(hdc, m_hFontSmall);
    SetTextColor(hdc, SmartColors::TEXT_SECONDARY);
    RECT labelRc = {15, y, 100, y + 20};
    DrawTextW(hdc, L"Disk:", -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    y += 22;

    if (m_disks.empty()) {
        SetTextColor(hdc, SmartColors::TEXT_DIM);
        RECT noDiskRc = {15, y, w - 30, y + 24};
        DrawTextW(hdc, L"No physical disks detected.", -1, &noDiskRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        y += 30;
        return;
    }

    size_t count = m_disks.size();
    int diskW = std::min(180, (w - 40) / std::max(static_cast<int>(count), 1));

    for (size_t i = 0; i < count; ++i) {
        int dx = 15 + static_cast<int>(i) * (diskW + 8);

        RECT tabRc = {dx, y, dx + diskW, y + 28};
        bool selected = (i == m_selectedDiskIndex);

        COLORREF bgCol = selected ? RGB(40, 42, 52) : SmartColors::BG_CARD;
        HBRUSH tabBg = CreateSolidBrush(bgCol);
        FillRect(hdc, &tabRc, tabBg);
        DeleteObject(tabBg);

        COLORREF borderCol = selected ? SmartColors::ACCENT_CYAN : SmartColors::BORDER;
        HPEN tabPen = CreatePen(PS_SOLID, selected ? 2 : 1, borderCol);
        HPEN oldP = (HPEN)SelectObject(hdc, tabPen);
        HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
        HBRUSH oldB = (HBRUSH)SelectObject(hdc, nullB);
        RoundRect(hdc, tabRc.left, tabRc.top, tabRc.right, tabRc.bottom, 4, 4);
        SelectObject(hdc, oldP);
        SelectObject(hdc, oldB);
        DeleteObject(tabPen);

        // Disk model (truncated)
        SetTextColor(hdc, selected ? SmartColors::TEXT_PRIMARY : SmartColors::TEXT_SECONDARY);
        RECT tabText = {dx + 8, y + 2, dx + diskW - 8, y + 14};

        std::wstring model = m_disks[i].identity.model;
        if (model.length() > 20) model = model.substr(0, 18) + L"..";
        DrawTextW(hdc, model.c_str(), -1, &tabText, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        // Interface type
        RECT tabSub = {dx + 8, y + 16, dx + diskW - 8, y + 26};
        std::wstring iface = ataToWide(m_disks[i].identity.interfaceName);
        if (iface.empty()) iface = L"Unknown";
        DrawTextW(hdc, iface.c_str(), -1, &tabSub, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    y += 32;
}

void SmartMonitor::drawRingGauge(HDC hdc, int cx, int cy, int radius, double percent,
                                   const wchar_t* /*label*/) {
    // Draw a ring/donut gauge for health percentage
    int thickness = 10;
    double angle = percent / 100.0 * 360.0;

    // Background ring
    HPEN bgPen = CreatePen(PS_SOLID, thickness, RGB(50, 52, 58));
    HPEN oldPen = (HPEN)SelectObject(hdc, bgPen);
    HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, nullBr);

    Ellipse(hdc, cx - radius, cy - radius, cx + radius, cy + radius);
    DeleteObject(bgPen);

    // Foreground arc
    COLORREF fgColor = healthColor(percent);
    HPEN fgPen = CreatePen(PS_SOLID, thickness, fgColor);
    SelectObject(hdc, fgPen);

    // Draw arc using Pie
    double startAngle = -90.0; // Start from top (12 o'clock)
    double endAngle = startAngle + angle;
    double radStart = startAngle * M_PI / 180.0;
    double radEnd = endAngle * M_PI / 180.0;

    int x1 = cx + static_cast<int>(radius * cos(radStart));
    int y1 = cy + static_cast<int>(radius * sin(radStart));
    int x2 = cx + static_cast<int>(radius * cos(radEnd));
    int y2 = cy + static_cast<int>(radius * sin(radEnd));

    Pie(hdc, cx - radius, cy - radius, cx + radius, cy + radius, x2, y2, x1, y1);

    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBr);
    DeleteObject(fgPen);
}

void SmartMonitor::drawMiniLineChart(HDC hdc, int x, int y, int w, int h,
                                      const std::vector<TempPoint>& data,
                                      double maxVal, COLORREF color) {
    if (data.size() < 2) return;

    double minVal = 20.0; // Minimum display temp
    double range = maxVal - minVal;
    if (range <= 0) range = 1;

    // Draw grid line at threshold
    HPEN gridPen = CreatePen(PS_DOT, 1, RGB(80, 40, 40));
    HPEN oldPen = (HPEN)SelectObject(hdc, gridPen);
    int thresholdY = y + h - 1 - static_cast<int>((m_highTempThreshold - minVal) / range * (h - 2));
    MoveToEx(hdc, x, thresholdY, nullptr);
    LineTo(hdc, x + w, thresholdY);
    DeleteObject(gridPen);

    // Draw line chart
    HPEN linePen = CreatePen(PS_SOLID, 2, color);
    SelectObject(hdc, linePen);

    double stepX = static_cast<double>(w) / std::max(static_cast<size_t>(1), data.size() - 1);

    MoveToEx(hdc, x, y + h - 1, nullptr);
    bool first = true;

    for (size_t i = 0; i < data.size(); ++i) {
        int px = x + static_cast<int>(i * stepX);
        double val = std::max(minVal, std::min(maxVal, data[i].celsius));
        int py = y + h - 1 - static_cast<int>((val - minVal) / range * (h - 2));

        if (first) {
            MoveToEx(hdc, px, py, nullptr);
            first = false;
        } else {
            LineTo(hdc, px, py);
        }
    }

    SelectObject(hdc, oldPen);
    DeleteObject(linePen);
}

void SmartMonitor::drawMiniBarChart(HDC hdc, int x, int y, int w, int h,
                                     const std::vector<double>& data,
                                     COLORREF color) {
    if (data.empty()) return;

    size_t count = data.size();
    double barW = static_cast<double>(w) / count;
    if (barW < 2) barW = 2;

    // Draw baseline at 50%
    HPEN basePen = CreatePen(PS_DOT, 1, RGB(80, 80, 40));
    HPEN oldPen = (HPEN)SelectObject(hdc, basePen);
    int baseY = y + h / 2;
    MoveToEx(hdc, x, baseY, nullptr);
    LineTo(hdc, x + w, baseY);
    DeleteObject(basePen);

    // Draw bars
    for (size_t i = 0; i < count; ++i) {
        int bx = x + static_cast<int>(i * barW);
        int barH = static_cast<int>((data[i] / 100.0) * h);
        COLORREF barColor = healthColor(data[i]);

        HBRUSH barBr = CreateSolidBrush(barColor);
        RECT barRc = {bx + 1, y + h - barH, bx + static_cast<int>(barW) - 1, y + h};
        FillRect(hdc, &barRc, barBr);
        DeleteObject(barBr);
    }

    SelectObject(hdc, oldPen);
}

// Additional helper: convert narrow string to wide
namespace {
    std::wstring _ataToWide(const std::string& s) {
        if (s.empty()) return L"";
        int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring result(len, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &result[0], len);
        return result;
    }
}

std::wstring SmartMonitor::ataToWide(const std::string& s) {
    return _ataToWide(s);
}

// =====================================================================
//  DEBUG WINDOW
// =====================================================================

void SmartMonitor::toggleDebugWindow() {
    if (m_debugWindowVisible) {
        SMART_TRACE_EVENT("toggleDebugWindow", SmartTraceCategory::USER_INTERACTION, "Closing debug window");
        if (m_debugWindow) {
            m_debugWindow->destroy();
            m_debugWindow.reset();
        }
        m_debugWindowVisible = false;
    } else {
        SMART_TRACE_EVENT("toggleDebugWindow", SmartTraceCategory::USER_INTERACTION, "Opening debug window");
        m_debugWindow = std::make_unique<SmartDebugWindow>();
        if (m_debugWindow->create(m_hwnd, m_hInst)) {
            m_debugWindowVisible = true;
            m_debugWindow->refresh();
        } else {
            SMART_TRACE_EVENT("toggleDebugWindow", SmartTraceCategory::ERROR_EXCEPTION, "Failed to create debug window");
            m_debugWindow.reset();
        }
    }
}

void SmartMonitor::updateDebugWindow() {
    if (m_debugWindowVisible && m_debugWindow) {
        m_debugWindow->refresh();
    }
}
