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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "Monitor.h"
#include "Display.h"
#include "Recorder.h"
#include "CdiSmart.h"
#include "CdiAttributeName.h"
#include "SmartCdiAdapter.h"
#include "SmartDebug.h"
#include <cstdio>
#include <cstdlib>
#include <atomic>

static std::atomic<bool> g_shutdown{false};

static BOOL WINAPI ctrlHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT ||
        type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        g_shutdown = true;
        return TRUE;
    }
    return FALSE;
}

// ── --smart-dump ─────────────────────────────────────────────────────
//
// Console diagnostic for the CrystalDiskInfo port: enumerates every physical
// drive, reads S.M.A.R.T. through the same code the GUI uses and prints the
// result.  This is how the acquisition layer is verified on real hardware
// without opening the SMART page and pressing keys.
static int runSmartDump() {
    wprintf(L"\n  IOMonitor  -  S.M.A.R.T. dump (CrystalDiskInfo port)\n");
    wprintf(L"  ==============================================================\n");

    // The port traces every detection and IOCTL step; keep that trace in a
    // log file so a failure here is diagnosable after the fact.
    SmartTraceBuffer::instance().startFileLogging();

    cdi::CAtaSmart ata;
    if (!ata.Init(FALSE)) {
        SmartTraceBuffer::instance().stopFileLogging();
        wprintf(L"\n  No disk found.\n\n");
        wprintf(L"  Reading S.M.A.R.T. opens \\\\.\\PhysicalDriveN with GENERIC_WRITE,\n");
        wprintf(L"  which requires an elevated token.  Re-run from an Administrator\n");
        wprintf(L"  command prompt.\n\n");
        wprintf(L"  Detection trace: %ls\n\n",
                SmartTraceBuffer::instance().getLogFilePath().c_str());
        return 1;
    }

    const DWORD count = ata.GetDiskCount();
    wprintf(L"  Found %u disk(s).\n", static_cast<unsigned>(count));

    for (DWORD i = 0; i < count; ++i) {
        // The first read runs before the vendor is known, so the
        // vendor-gated attribute parse only lands on the refresh that
        // follows.  CrystalDiskInfo's own UI does the same.
        ata.UpdateSmartInfo(i);

        const cdi::DRIVE_INFO& d = ata.GetDisk(i);

        wprintf(L"\n  --------------------------------------------------------------\n");
        wprintf(L"  Disk %u   PhysicalDrive%d   target 0x%02X\n",
                static_cast<unsigned>(i), d.PhysicalDriveId, d.Target);
        wprintf(L"    Model           : %ls\n", d.Model.c_str());
        wprintf(L"    Serial          : %ls\n", d.SerialNumber.c_str());
        wprintf(L"    Firmware        : %ls\n", d.FirmwareRev.c_str());
        wprintf(L"    Interface       : %ls\n", d.Interface.c_str());
        wprintf(L"    Transfer mode   : %ls\n", d.TransferMode.c_str());
        wprintf(L"    Command type    : %ls%ls\n", d.CommandTypeString.c_str(),
                d.IsNVMe ? L"  (NVMe)" : L"");
        // ATA drives report a sector count; NVMe drives carry TotalDiskSize
        // (in 10^6 bytes) instead, because the namespace LBA count comes from
        // the identify namespace log rather than the ATA geometry.
        const unsigned long long capacityBytes =
            d.NumberOfSectors != 0
                ? static_cast<unsigned long long>(d.NumberOfSectors) * d.LogicalSectorSize
                : static_cast<unsigned long long>(d.TotalDiskSize) * 1000000ULL;
        wprintf(L"    Capacity        : %llu bytes  (%llu GB)\n",
                capacityBytes, capacityBytes / (1000ULL * 1000 * 1000));
        wprintf(L"    Logical sector  : %u bytes\n", d.LogicalSectorSize);
        wprintf(L"    Vendor id / key : %u / %ls   [%ls]\n",
                static_cast<unsigned>(d.DiskVendorId), d.SmartKeyName.c_str(),
                d.SsdVendorString.c_str());
        wprintf(L"    Form factor     : %ls\n", d.DeviceNominalFormFactor.c_str());

        const wchar_t* verdict = L"UNKNOWN";
        switch (d.DiskStatus) {
        case cdi::DISK_STATUS_GOOD:    verdict = L"GOOD"; break;
        case cdi::DISK_STATUS_CAUTION: verdict = L"CAUTION"; break;
        case cdi::DISK_STATUS_BAD:     verdict = L"BAD"; break;
        default: break;
        }
        wprintf(L"    Health verdict  : %ls\n", verdict);
        wprintf(L"    S.M.A.R.T.      : supported=%ls enabled=%ls data=%ls thresholds=%ls\n",
                d.IsSmartSupported ? L"yes" : L"no",
                d.IsSmartEnabled ? L"yes" : L"no",
                d.IsSmartCorrect ? L"ok" : L"unavailable",
                d.IsThresholdCorrect ? L"ok" : L"unavailable");
        wprintf(L"    Temperature     : %d C\n", d.Temperature);
        wprintf(L"    Power-on hours  : %d   (power cycles: %u)\n",
                d.PowerOnHours, static_cast<unsigned>(d.PowerOnCount));
        wprintf(L"    Host read/write : %llu / %llu bytes\n",
                static_cast<unsigned long long>(d.HostReadsBytes),
                static_cast<unsigned long long>(d.HostWritesBytes));
        wprintf(L"    Life remaining  : %d %%   (wear levelling: %d)\n",
                d.Life, d.WearLevelingCount);

        if (d.AttributeCount == 0) {
            wprintf(L"\n    (no S.M.A.R.T. attributes available)\n");
            continue;
        }

        wprintf(L"\n    %-4ls %-36ls %5ls %5ls %5ls  %ls\n",
                L"ID", L"Name", L"Cur", L"Wor", L"Thr", L"Raw");
        wprintf(L"    --------------------------------------------------------------------\n");

        for (DWORD a = 0; a < d.AttributeCount && a < cdi::MAX_ATTRIBUTE; ++a) {
            const cdi::SMART_ATTRIBUTE& attr = d.Attribute[a];
            if (attr.Id == 0) continue;

            const std::wstring name = cdi::GetSmartAttributeName(d, attr.Id);
            const std::wstring raw = cdi::FormatSmartRawValue(d, attr);
            const cdi::SMART_THRESHOLD& thr = d.Threshold[a];
            const BYTE threshold = (thr.Id == attr.Id) ? thr.ThresholdValue : 0;

            wprintf(L"    %02X   %-36ls %5u %5u %5u  %ls\n",
                    attr.Id, name.c_str(), attr.CurrentValue, attr.WorstValue,
                    threshold, raw.c_str());
        }
    }

    // Feed the same conversion the GUI uses, so a regression in the adapter
    // shows up here rather than only in the SMART page.
    wprintf(L"\n  --------------------------------------------------------------\n");
    wprintf(L"  As the SMART page sees it (SmartCdiAdapter)\n");

    for (DWORD i = 0; i < count; ++i) {
        const cdi::DRIVE_INFO& d = ata.GetDisk(i);

        SmartDataSnapshot snap;
        smartcdi::FillSnapshot(d, i, snap);

        const wchar_t* bus = L"Unknown";
        switch (snap.identity.diskInterface) {
        case DiskInterfaceType::ATA:   bus = L"ATA";   break;
        case DiskInterfaceType::NVMe:  bus = L"NVMe";  break;
        case DiskInterfaceType::SCSI:  bus = L"SCSI";  break;
        default: break;
        }

        const wchar_t* verdict = L"not checked";
        if (snap.smartReturnStatus == 0) verdict = L"good";
        else if (snap.smartReturnStatus == 1) verdict = L"caution or worse";

        wprintf(L"    Disk %u : %ls\n", static_cast<unsigned>(i), snap.identity.model.c_str());
        wprintf(L"             bus=%ls iface=%hs capacity=%llu bytes sector=%u rotation=%d\n",
                bus, snap.identity.interfaceName.c_str(),
                static_cast<unsigned long long>(snap.identity.capacityBytes),
                snap.identity.sectorSize, snap.identity.rotationRate);
        wprintf(L"             smartSupported=%d smartEnabled=%d dataValid=%d\n",
                snap.identity.smartSupported ? 1 : 0,
                snap.identity.smartEnabled ? 1 : 0,
                snap.dataValid ? 1 : 0);
        wprintf(L"             attributes=%zu  temp=%.1f C  powerOnHours=%llu  life=%lld%%\n",
                snap.attributes.size(), snap.temperatureCelsius,
                static_cast<unsigned long long>(snap.powerOnHours),
                static_cast<long long>(snap.remainingLifePercent));
        wprintf(L"             bytes read/written=%llu / %llu  wear=%lld  verdict=%ls\n",
                static_cast<unsigned long long>(snap.totalBytesRead),
                static_cast<unsigned long long>(snap.totalBytesWritten),
                static_cast<long long>(snap.wearLevelingCount), verdict);

        // The columns the SMART page paints for each row.
        wprintf(L"             %3ls %-32ls %5ls %5ls %5ls  %ls\n",
                L"ID", L"Attribute Name", L"Value", L"Worst", L"Thresh", L"Raw");
        for (const SmartAttribute& a : snap.attributes) {
            wprintf(L"             %3u %-32hs %5u %5u %5u  %hs\n",
                    a.id, a.name.c_str(), a.current, a.worst, a.threshold,
                    a.rawString.c_str());
        }
    }

    SmartTraceBuffer::instance().stopFileLogging();
    wprintf(L"\n  Trace log: %ls\n\n",
            SmartTraceBuffer::instance().getLogFilePath().c_str());
    return 0;
}

static void printHelp() {
    wprintf(L"\n  IO Monitor  —  Disk I/O Usage Monitor v3.0\n\n");
    wprintf(L"  Usage:  iomonitor [options]\n\n");
    wprintf(L"  Options:\n");
    wprintf(L"    -s, --sample N     Sampling interval in ms   (default: 1000, range: 200-10000)\n");
    wprintf(L"    -r, --refresh N    Display refresh in ms     (default: 500,  range: 100-2000)\n");
    wprintf(L"    -n, --num N        Max processes to display  (default: 30,   range: 5-100)\n");
    wprintf(L"    -o, --record       Start recording to CSV on launch\n");
    wprintf(L"    -h, --help         Show this help\n");
    wprintf(L"        --smart-dump   Print every disk's S.M.A.R.T. data and exit\n");
    wprintf(L"                       (needs an Administrator command prompt)\n\n");
    wprintf(L"  Keyboard controls:\n");
    wprintf(L"    Q / Esc          Quit\n");
    wprintf(L"    R                Sort by Read rate\n");
    wprintf(L"    W                Sort by Write rate\n");
    wprintf(L"    T                Sort by Total rate\n");
    wprintf(L"    S                Sort by Session I/O total\n");
    wprintf(L"    P                Sort by Process lifetime I/O\n");
    wprintf(L"    C                Clear session totals\n");
    wprintf(L"    O                Toggle CSV recording on/off\n");
    wprintf(L"    M                Toggle overlay mini-window (always-on-top, 25%% opaque)\n");
    wprintf(L"    D                Open SMART disk health monitor sub-page\n");
    wprintf(L"    1-5              Sample speed presets (200/500/1000/2000/5000 ms)\n");
    wprintf(L"    +/-              Increase / decrease displayed process count\n");
    wprintf(L"    [ / ]            Adjust display refresh speed\n\n");
    wprintf(L"  Run with high integrity (administrator) for complete process coverage.\n");
    wprintf(L"  Recording files are saved to: <exe_dir>/records/IO_YYYYMMDD_HHMMSS.csv\n\n");
}

int wmain(int argc, wchar_t* argv[]) {
    int sampleMs  = 1000;
    int refreshMs = 500;
    int numProc   = 30;
    bool autoRecord = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"-h" || arg == L"--help") { printHelp(); return 0; }
        if (arg == L"--smart-dump")           { return runSmartDump(); }

        auto nextInt = [&]() -> int {
            if (i + 1 < argc) return _wtoi(argv[++i]);
            return -1;
        };

        if      (arg == L"-s" || arg == L"--sample")  { int v = nextInt(); if (v >= 200 && v <= 10000) sampleMs  = v; }
        else if (arg == L"-r" || arg == L"--refresh") { int v = nextInt(); if (v >= 100 && v <= 2000)  refreshMs = v; }
        else if (arg == L"-n" || arg == L"--num")     { int v = nextInt(); if (v >= 5   && v <= 100)   numProc   = v; }
        else if (arg == L"-o" || arg == L"--record")  { autoRecord = true; }
    }

    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    // Initialize recorder
    Recorder recorder;
    if (autoRecord) {
        if (!recorder.start()) {
            fwprintf(stderr, L"Warning: Failed to start CSV recording.\n");
        } else {
            wprintf(L"Recording started: %ls\n", recorder.getFilePath().c_str());
        }
    }

    DiskMonitor monitor;
    if (!monitor.start(sampleMs)) {
        fwprintf(stderr, L"Failed to start disk monitor.\n");
        recorder.stop();
        return 1;
    }

    // Let the first sample complete for a valid baseline
    Sleep(static_cast<DWORD>(sampleMs + 200));

    ConsoleDisplay display;
    display.setRefreshMs(refreshMs);
    display.setMaxDisplay(numProc);
    display.run(monitor, recorder);

    monitor.stop();
    recorder.stop();
    SetConsoleCtrlHandler(ctrlHandler, FALSE);
    return 0;
}
