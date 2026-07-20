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

static void printHelp() {
    wprintf(L"\n  IO Monitor  —  Disk I/O Usage Monitor v1.0\n\n");
    wprintf(L"  Usage:  iomonitor [options]\n\n");
    wprintf(L"  Options:\n");
    wprintf(L"    -s, --sample N     Sampling interval in ms   (default: 1000, range: 200-10000)\n");
    wprintf(L"    -r, --refresh N    Display refresh in ms     (default: 500,  range: 100-2000)\n");
    wprintf(L"    -n, --num N        Max processes to display  (default: 30,   range: 5-100)\n");
    wprintf(L"    -o, --record       Start recording to CSV on launch\n");
    wprintf(L"    -h, --help         Show this help\n\n");
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
            wprintf(L"Recording started: %s\n", recorder.getFilePath().c_str());
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
