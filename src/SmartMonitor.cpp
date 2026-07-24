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
#include "SmartReaderAta.h"
#include "SmartReaderNvme.h"
#include "SmartReaderScsi.h"
#include "SmartDebugWindow.h"
#include <winioctl.h>
#include <algorithm>
#include <cstdio>
#include <cmath>
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

void SmartMonitor::enumerateDisks() {
    SMART_TRACE_SCOPE("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, "Starting disk enumeration");

    m_disks.clear();
    m_selectedDiskIndex = 0;

    int foundCount = 0;
    int smartCount = 0;

    for (uint32_t i = 0; i < 32; ++i) {
        wchar_t path[64];
        swprintf(path, 64, L"\\\\.\\PhysicalDrive%u", i);

        HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        CloseHandle(h);

        DiskEntry entry;
        entry.identity.diskNumber = i;

        // Try to read identity using the correct reader
        auto reader = createReaderForDisk(i);
        if (reader) {
            if (reader->readIdentity(entry.identity)) {
                entry.smartAvailable = true;
                smartCount++;

                char msg[256];
                snprintf(msg, sizeof(msg), "Disk %u: model=%s interface=%s smart=%s",
                         i,
                         std::string(entry.identity.model.begin(), entry.identity.model.end()).c_str(),
                         entry.identity.interfaceName.c_str(),
                         entry.smartAvailable ? "yes" : "no");
                SMART_TRACE_EVENT("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, msg);
            }
            reader->close();
        }

        if (entry.identity.model.empty()) {
            wchar_t buf[32];
            swprintf(buf, 32, L"PhysicalDrive%u", i);
            entry.identity.model = buf;
        }

        m_disks.push_back(entry);
        foundCount++;
    }

    char summary[128];
    snprintf(summary, sizeof(summary), "Enumeration complete: %d disks found, %d SMART-capable", foundCount, smartCount);
    SMART_TRACE_EVENT("enumerateDisks", SmartTraceCategory::DISK_DISCOVERY, summary);
}

// ── Reader creation: detect bus type and instantiate correct reader ──

std::unique_ptr<SmartReaderBase> SmartMonitor::createReaderForDisk(uint32_t diskNumber) {
    SMART_TRACE_BEGIN("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY,
                      "Creating reader for disk");

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%u", diskNumber);

    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "Failed to open device");
        return nullptr;
    }

    std::vector<uint8_t> propBuf(sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512, 0);
    DWORD bytesReturned = 0;

    BOOL propOk = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                                   &query, sizeof(query),
                                   propBuf.data(), static_cast<DWORD>(propBuf.size()),
                                   &bytesReturned, nullptr);
    CloseHandle(h);

    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    if (propOk && bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(propBuf.data());
        busType = desc->BusType;
    }

    // Bus type names for debug (BusTypeNvme = 17 per Windows SDK)
    const char* busName = "Unknown";
    switch (busType) {
    case BusTypeUnknown: busName = "Unknown"; break;
    case BusTypeScsi:    busName = "SCSI"; break;
    case BusTypeAtapi:   busName = "ATAPI"; break;
    case BusTypeAta:     busName = "ATA"; break;
    case 4:              busName = "1394"; break;
    case 5:              busName = "SSA"; break;
    case 6:              busName = "Fibre"; break;
    case BusTypeUsb:     busName = "USB"; break;
    case BusTypeRAID:    busName = "RAID"; break;
    case 9:              busName = "iSCSI"; break;
    case BusTypeSas:     busName = "SAS"; break;
    case BusTypeSata:    busName = "SATA"; break;
    case BusTypeSd:      busName = "SD"; break;
    case BusTypeMmc:     busName = "MMC"; break;
    case BusTypeVirtual: busName = "Virtual"; break;
    case BusTypeFileBackedVirtual: busName = "FileVirtual"; break;
    case 17:             busName = "NVMe"; break;
    default:             busName = "Other"; break;
    }

    char msg[128];
    snprintf(msg, sizeof(msg), "BusType=%s(%d)", busName, static_cast<int>(busType));
    SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, msg);

    // Route to correct reader based on bus type
    if (busType == BusTypeNvme) {
        SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "Routing to SmartReaderNvme");
        auto nvme = std::make_unique<SmartReaderNvme>();
        if (nvme->open(diskNumber)) {
            DiskIdentity id;
            if (nvme->readIdentity(id) && !id.model.empty()) {
                SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "NVMe reader created successfully");
                return nvme;
            }
            nvme->close();
        }
        SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "NVMe reader failed");
    }

    if (busType == BusTypeAta || busType == BusTypeSata ||
        busType == BusTypeSas || busType == BusTypeRAID ||
        busType == BusTypeUsb || busType == BusTypeUnknown) {
        SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "Routing to SmartReaderAta");
        auto ata = std::make_unique<SmartReaderAta>();
        if (ata->open(diskNumber)) {
            DiskIdentity id;
            if (ata->readIdentity(id) && !id.model.empty()) {
                SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "ATA reader created");
                return ata;
            }
            ata->close();
        }
        SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "ATA reader failed");
    }

    // Fallback: SCSI (USB bridges, etc.)
    SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "Fallback to SmartReaderScsi");
    auto scsi = std::make_unique<SmartReaderScsi>();
    if (scsi->open(diskNumber)) {
        DiskIdentity id;
        if (scsi->readIdentity(id) && !id.model.empty()) {
            SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "SCSI reader created");
            return scsi;
        }
        scsi->close();
    }

    // Last resort: try ATA anyway (for virtual disks, etc.)
    SMART_TRACE_EVENT("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "Last resort: ATA retry");
    auto ata = std::make_unique<SmartReaderAta>();
    if (ata->open(diskNumber)) {
        DiskIdentity id;
        if (ata->readIdentity(id) && !id.model.empty()) {
            SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "ATA reader created (last resort)");
            return ata;
        }
        ata->close();
    }

    SMART_TRACE_END("createReaderForDisk", SmartTraceCategory::DISK_DISCOVERY, "No suitable reader found");
    return nullptr;
}

// ── SMART data refresh ───────────────────────────────────────────────

void SmartMonitor::refreshSmartData() {
    if (m_selectedDiskIndex >= m_disks.size()) return;

    auto& entry = m_disks[m_selectedDiskIndex];
    SmartDataSnapshot newSnap;

    char msg[128];
    snprintf(msg, sizeof(msg), "Refreshing SMART data for disk %u", entry.identity.diskNumber);
    SMART_TRACE_BEGIN("refreshSmartData", SmartTraceCategory::DATA_REFRESH, msg);

    bool ok = readSmartDataForDisk(entry.identity.diskNumber, newSnap);
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

bool SmartMonitor::readSmartDataForDisk(uint32_t diskNumber, SmartDataSnapshot& snapshot) {
    SMART_TRACE_BEGIN("readSmartDataForDisk", SmartTraceCategory::DATA_REFRESH, "Opening reader");

    auto reader = createReaderForDisk(diskNumber);
    if (!reader) {
        SMART_TRACE_END("readSmartDataForDisk", SmartTraceCategory::ERROR_EXCEPTION, "No reader available");
        snapshot.dataValid = false;
        snapshot.errorMessage = "Cannot open disk device. Check permissions.";
        snapshot.permissionHint = L"Run as Administrator for full SMART access.";
        return false;
    }

    // Read identity
    SMART_TRACE_EVENT("readSmartDataForDisk", SmartTraceCategory::DATA_REFRESH, "Reading disk identity");
    if (!reader->readIdentity(snapshot.identity)) {
        SMART_TRACE_EVENT("readSmartDataForDisk", SmartTraceCategory::ERROR_EXCEPTION, "readIdentity failed");
        snapshot.dataValid = false;
        snapshot.errorMessage = "Failed to read disk identity.";
        reader->close();
        return false;
    }

    // Read attributes
    SMART_TRACE_EVENT("readSmartDataForDisk", SmartTraceCategory::DATA_REFRESH, "Reading SMART attributes");
    std::vector<SmartAttribute> attrs;
    if (!reader->readAttributes(attrs)) {
        SMART_TRACE_EVENT("readSmartDataForDisk", SmartTraceCategory::ERROR_EXCEPTION,
                         "readAttributes failed - both pass-through and protocol query paths failed");
        snapshot.dataValid = false;
        snapshot.errorMessage = "Failed to read SMART attributes. Device may not support SMART or requires admin privileges.";
        snapshot.permissionHint = L"Ensure the device supports S.M.A.R.T. and run as Administrator.";
        reader->close();
        return false;
    }

    // Read thresholds
    reader->readThresholds(attrs);

    // SMART RETURN STATUS check - smartmontools' ataSmartStatus2()
    // This is the definitive check for whether any SMART threshold has been exceeded.
    // It provides the same "SMART overall-health self-assessment test" result
    // that BIOS/UEFI uses to determine if a disk is failing.
    snapshot.smartReturnStatus = reader->checkSmartStatus();

    snapshot.attributes = std::move(attrs);
    reader->close();

    // Extract key metrics from attributes
    for (auto& attr : snapshot.attributes) {
        switch (attr.id) {
        case 9: // Power-On Hours
            snapshot.powerOnHours = attr.rawValue;
            break;
        case 194: // Temperature Celsius
            snapshot.temperatureCelsius = static_cast<double>(attr.rawValue & 0xFF);
            break;
        case 190: // Airflow Temperature (alternate)
            if (snapshot.temperatureCelsius == 0.0) {
                snapshot.temperatureCelsius = static_cast<double>(attr.rawValue & 0xFF);
            }
            break;
        case 241: // Total LBAs Written
            snapshot.totalLbasWritten = attr.rawValue;
            break;
        case 242: // Total LBAs Read
            snapshot.totalLbasRead = attr.rawValue;
            break;
        case 177: // Wear Leveling Count
            snapshot.wearLevelingCount = static_cast<int64_t>(attr.rawValue);
            break;
        case 202: // Percentage of Rated Life Used / Remaining
            snapshot.remainingLifePercent = static_cast<int64_t>(attr.rawValue);
            break;
        }
    }

    // Convert to bytes: NVMe Data Units are 512*1000 bytes, ATA uses sectors
    if (snapshot.identity.diskInterface == DiskInterfaceType::NVMe) {
        snapshot.totalBytesRead = snapshot.totalLbasRead * 512000ULL;
        snapshot.totalBytesWritten = snapshot.totalLbasWritten * 512000ULL;
    } else {
        uint32_t sectorSize = snapshot.identity.sectorSize > 0 ? snapshot.identity.sectorSize : 512;
        snapshot.totalBytesRead = snapshot.totalLbasRead * sectorSize;
        snapshot.totalBytesWritten = snapshot.totalLbasWritten * sectorSize;
    }

    // NVMe: rawValue stored in Kelvin — extract Celsius from rawString
    if (snapshot.identity.diskInterface == DiskInterfaceType::NVMe) {
        for (auto& attr : snapshot.attributes) {
            if (attr.id == 194 || attr.name == "Temperature") {
                double t = 0;
                std::wstring tempWide = ataToWide(attr.rawString);
                if (swscanf_s(tempWide.c_str(), L"%lf", &t) == 1) {
                    snapshot.temperatureCelsius = t;
                }
                break;
            }
        }
    }

    // For NVMe: Percentage Used is remaining life
    if (snapshot.remainingLifePercent < 0) {
        for (auto& attr : snapshot.attributes) {
            if (attr.id == 202 || attr.name == "Percentage Used") {
                snapshot.remainingLifePercent = static_cast<int64_t>(attr.rawValue);
                break;
            }
        }
    }

    // Session power-on hours (estimate from session start time)
    auto now = std::chrono::steady_clock::now();
    snapshot.sessionPowerOnHours = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(now - m_sessionStart).count() / 3600);

    snapshot.dataValid = true;

    char msg[128];
    snprintf(msg, sizeof(msg), "Success: %zu attributes, temp=%.1fC, health=%.1f%%",
             snapshot.attributes.size(), snapshot.temperatureCelsius, snapshot.healthPercent);
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

    // For ATA drives, SMART RETURN STATUS failure is definitive ("drive is failing").
    // For NVMe, critical warnings are transient (e.g. temperature threshold) and
    // are handled through the weighted attribute system below.
    if (snapshot.smartReturnStatus == 1 &&
        snapshot.identity.diskInterface != DiskInterfaceType::NVMe) {
        snapshot.healthPercent = 0.0;
        snapshot.status = SmartStatus::Failed;
        SMART_TRACE_END("computeHealth", SmartTraceCategory::HEALTH_COMPUTE,
                        "FAILED: SMART RETURN STATUS indicates threshold exceeded");
        return;
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

        // NVMe-specific health factors
        if (snapshot.identity.diskInterface == DiskInterfaceType::NVMe) {
            if (attr.name == "Critical Warning") {
                weight = std::max(weight, SmartHealthWeights::NVME_CRITICAL_WARNING);
            }
            if (attr.name == "Media Errors") {
                weight = std::max(weight, SmartHealthWeights::NVME_MEDIA_ERRORS);
            }
            if (attr.name == "Percentage Used") {
                weight = std::max(weight, SmartHealthWeights::NVME_PERCENTAGE_USED);
            }
            if (attr.name == "Unsafe Shutdowns") {
                weight = std::max(weight, SmartHealthWeights::NVME_UNSAFE_SHUTDOWNS);
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
    if (v >= 100.0) swprintf(buf, 64, L"%.0f %s", v, units[i]);
    else if (v >= 10.0) swprintf(buf, 64, L"%.1f %s", v, units[i]);
    else swprintf(buf, 64, L"%.2f %s", v, units[i]);
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
        RECT leftRc = {10, y, leftW, h - 40};
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

    // Center + Right area
    y += 5;
    int contentH = h - y - 40;

    // Center: Metric cards
    {
        int cardW = (centerW - 20) / 2;
        int cardH = (contentH - 20) / 2;

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
            swprintf(rBuf, 64, L"R: %s", fmtBytesSmart(snap.totalBytesRead).c_str());
            DrawTextW(memDC, rBuf, -1, &rRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Write
            SetTextColor(memDC, SmartColors::WRITE_COLOR);
            RECT wRc = {cx + 12, cy + 56, cx + cardW - 12, cy + 78};
            wchar_t wBuf[64];
            swprintf(wBuf, 64, L"W: %s", fmtBytesSmart(snap.totalBytesWritten).c_str());
            DrawTextW(memDC, wBuf, -1, &wRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Rates
            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT rateRc = {cx + 12, cy + 78, cx + cardW - 12, cy + 96};
            wchar_t rateBuf[64];
            swprintf(rateBuf, 64, L"Rate: R %s  W %s",
                     fmtRateMBps(snap.readRateMBps).c_str(),
                     fmtRateMBps(snap.writeRateMBps).c_str());
            DrawTextW(memDC, rateBuf, -1, &rateRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    // Right: Charts + Detailed Attributes
    {
        int rx = w - rightW - 5;
        int ry = y;

        // Health history bar chart
        {
            RECT chartRc = {rx, ry, rx + rightW, ry + 160};
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
            drawMiniBarChart(memDC, rx + 16, ry + 28, rightW - 32, 120, hhist, SmartColors::HEALTH_GREEN);
        }

        ry += 168;

        // Read/Write rate sparkline
        {
            RECT chartRc = {rx, ry, rx + rightW, ry + 120};
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
            swprintf(rrBuf, 64, L"Read:  %s", fmtRateMBps(snap.readRateMBps).c_str());
            DrawTextW(memDC, rrBuf, -1, &rrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            SetTextColor(memDC, SmartColors::WRITE_COLOR);
            RECT wrRc = {rx + 16, ry + 56, rx + rightW - 16, ry + 78};
            wchar_t wrBuf[64];
            swprintf(wrBuf, 64, L"Write: %s", fmtRateMBps(snap.writeRateMBps).c_str());
            DrawTextW(memDC, wrBuf, -1, &wrRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        ry += 128;

        // Detailed attributes panel
        if (!snap.attributes.empty()) {
            int attrH = h - ry - 10;
            RECT attrRc = {rx, ry, rx + rightW, ry + attrH};
            HBRUSH cardBg = CreateSolidBrush(SmartColors::BG_CARD);
            FillRect(memDC, &attrRc, cardBg);
            DeleteObject(cardBg);

            HPEN borderP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            HPEN oldP = (HPEN)SelectObject(memDC, borderP);
            HBRUSH nullB = (HBRUSH)GetStockObject(NULL_BRUSH);
            HBRUSH oldB = (HBRUSH)SelectObject(memDC, nullB);
            Rectangle(memDC, attrRc.left, attrRc.top, attrRc.right, attrRc.bottom);
            SelectObject(memDC, oldP); SelectObject(memDC, oldB);
            DeleteObject(borderP);

            SelectObject(memDC, m_hFontSmall);
            SetTextColor(memDC, SmartColors::TEXT_SECONDARY);
            RECT tRc = {rx + 12, ry + 6, rx + rightW - 12, ry + 24};
            DrawTextW(memDC, L"SMART Attributes", -1, &tRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Column headers
            int ay = ry + 28;
            SetTextColor(memDC, SmartColors::TEXT_DIM);
            RECT hdrRc = {rx + 8, ay, rx + rightW - 8, ay + 16};
            DrawTextW(memDC, L"ID  Attribute Name               Value Worst Thresh  Raw", -1, &hdrRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            ay += 18;

            // Separator
            HPEN sepP = CreatePen(PS_SOLID, 1, SmartColors::BORDER);
            SelectObject(memDC, sepP);
            MoveToEx(memDC, rx + 8, ay, nullptr);
            LineTo(memDC, rx + rightW - 8, ay);
            DeleteObject(sepP);

            // List attributes
            SelectObject(memDC, m_hFontMono);
            int maxRows = (attrH - 60) / 18;
            for (size_t i = 0; i < snap.attributes.size() && i < static_cast<size_t>(maxRows); ++i) {
                auto& attr = snap.attributes[i];
                ay += 2;

                COLORREF textColor = SmartColors::TEXT_PRIMARY;
                if (attr.preFailure && attr.current <= attr.threshold) {
                    textColor = SmartColors::HEALTH_RED;
                } else if (attr.preFailure) {
                    textColor = SmartColors::HEALTH_YELLOW;
                }

                SetTextColor(memDC, textColor);
                RECT attrR = {rx + 8, ay, rx + rightW - 8, ay + 16};
                wchar_t line[256];
                std::wstring rawW = ataToWide(attr.rawString);
                if (rawW.length() > 25) rawW = rawW.substr(0, 23) + L"..";
                swprintf(line, 256, L"%3d %-28.28s %3d  %3d  %3d   %s",
                         attr.id, ataToWide(attr.name).c_str(),
                         attr.current, attr.worst, attr.threshold,
                         rawW.c_str());
                DrawTextW(memDC, line, -1, &attrR, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                ay += 16;
            }
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
