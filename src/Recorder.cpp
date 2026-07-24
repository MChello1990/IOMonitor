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

#include "Recorder.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <ctime>
#include <sstream>
#include <iomanip>

#ifdef _MSC_VER
#pragma comment(lib, "shlwapi.lib")
#endif

Recorder::Recorder() {
}

Recorder::~Recorder() {
    stop();
}

// ── Get the directory where the executable resides ──────────────────
static std::wstring getExeDir() {
    wchar_t path[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";

    // Strip executable name to get directory
    wchar_t* lastSlash = wcsrchr(path, L'\\');
    if (lastSlash) *lastSlash = L'\0';
    return path;
}

// ── Ensure records/ directory exists ────────────────────────────────
bool Recorder::ensureRecordsDir() {
    m_dirPath = getExeDir() + L"\\records";
    
    // Try to create directory (succeeds if already exists)
    if (!CreateDirectoryW(m_dirPath.c_str(), nullptr)) {
        DWORD err = GetLastError();
        if (err != ERROR_ALREADY_EXISTS) {
            // Check if it exists as a file (not a directory)
            DWORD attrs = GetFileAttributesW(m_dirPath.c_str());
            if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
                // Path exists but is a file, not a directory — fail
                return false;
            }
            // Other error — fail
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                return false;
            }
        }
    }
    return true;
}

// ── Start recording ─────────────────────────────────────────────────
bool Recorder::start() {
    if (m_recording.load()) return true; // already recording

    if (!ensureRecordsDir()) {
        return false;
    }

    // Generate filename: records/IO_YYYYMMDD_HHMMSS.csv
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm;
    localtime_s(&tm, &t);

    wchar_t fname[128];
    swprintf(fname, 128, L"\\IO_%04d%02d%02d_%02d%02d%02d.csv",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
    m_filePath = m_dirPath + fname;

    // Open file for writing (UTF-8 with BOM for Excel compatibility)
    errno_t err = _wfopen_s(&m_file, m_filePath.c_str(), L"wb, ccs=UTF-8");
    if (err != 0 || !m_file) {
        return false;
    }

    // Write CSV header
    const char* header =
        "Timestamp,PID,ProcessName,ProcessPath,"
        "ReadRate(B/s),WriteRate(B/s),TotalRate(B/s),"
        "SessionReadBytes,SessionWriteBytes,SessionTotalBytes,"
        "ProcessReadBytes,ProcessWriteBytes,"
        "Event\n";
    fwrite(header, 1, strlen(header), m_file);
    fflush(m_file);

    // Mark recording as active BEFORE starting writer thread
    m_recording = true;
    m_stopWriter = false;

    // Start writer thread
    m_writerThread = std::thread(&Recorder::writerLoop, this);

    // Write a "start" marker
    RecordEntry startEntry;
    startEntry.timestamp = now;
    startEntry.event = "start";
    writeEntry(startEntry);

    return true;
}

// ── Stop recording ──────────────────────────────────────────────────
void Recorder::stop() {
    if (!m_recording.load()) return;

    // Write "stop" marker
    RecordEntry stopEntry;
    stopEntry.timestamp = std::chrono::system_clock::now();
    stopEntry.event = "stop";
    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        m_queue.push(std::move(stopEntry));
    }
    m_queueCV.notify_one();

    // Signal writer thread to stop after draining the queue
    m_stopWriter = true;
    m_queueCV.notify_one();

    if (m_writerThread.joinable()) {
        m_writerThread.join();
    }

    // Close file
    if (m_file) {
        fclose(m_file);
        m_file = nullptr;
    }

    m_recording = false;
}

// ── Enqueue a sample snapshot ───────────────────────────────────────
void Recorder::enqueueSample(const std::vector<ProcessIOData>& processes) {
    if (!m_recording.load()) return;

    RecordEntry entry;
    entry.timestamp = std::chrono::system_clock::now();
    entry.processes = processes;
    entry.event = "sample";

    {
        std::lock_guard<std::mutex> lk(m_queueMutex);
        // Limit queue size to prevent memory blowup if writer is slow
        // Drop oldest entries if queue exceeds 200
        if (m_queue.size() >= 200) {
            m_queue.pop(); // drop oldest
        }
        m_queue.push(std::move(entry));
    }
    m_queueCV.notify_one();
}

// ── Writer thread: drains queue and writes to CSV ───────────────────
void Recorder::writerLoop() {
    while (!m_stopWriter.load()) {
        RecordEntry entry;
        bool hasEntry = false;

        {
            std::unique_lock<std::mutex> lk(m_queueMutex);
            m_queueCV.wait(lk, [this] {
                return !m_queue.empty() || m_stopWriter.load();
            });

            if (m_queue.empty()) {
                if (m_stopWriter.load()) break;
                continue;
            }

            entry = std::move(m_queue.front());
            m_queue.pop();
            hasEntry = true;
        }

        if (hasEntry) {
            writeEntry(entry);
        }
    }

    // Drain remaining entries before exiting
    while (true) {
        RecordEntry entry;
        bool hasEntry = false;
        {
            std::lock_guard<std::mutex> lk(m_queueMutex);
            if (m_queue.empty()) break;
            entry = std::move(m_queue.front());
            m_queue.pop();
            hasEntry = true;
        }
        if (hasEntry) {
            writeEntry(entry);
        }
    }
}

// ── Write a single entry to CSV file ────────────────────────────────
void Recorder::writeEntry(const RecordEntry& entry) {
    if (!m_file) return;

    // For "start" and "stop" events, write a single line with metadata
    if (entry.event == "start" || entry.event == "stop") {
        std::string ts = formatTimestamp(entry.timestamp);
        std::string line = ts + ",,,,0,0,0,0,0,0,0,0," + entry.event + "\n";
        fwrite(line.c_str(), 1, line.size(), m_file);
        fflush(m_file);
        return;
    }

    // For "sample" events, write one line per process
    size_t rank = 0;
    for (const auto& proc : entry.processes) {
        // Only record processes with actual activity to reduce file size
        if (proc.totalRate() <= 0.0 && proc.totalSessionIO() == 0) {
            continue;
        }
        std::string line = formatCSVLine(entry, proc, ++rank);
        fwrite(line.c_str(), 1, line.size(), m_file);
    }

    // Flush periodically — important to prevent data loss on crash
    fflush(m_file);
}

// ── Format a single CSV line for a process ──────────────────────────
std::string Recorder::formatCSVLine(const RecordEntry& entry,
                                     const ProcessIOData& proc,
                                     size_t rank) const {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2);

    // Timestamp
    oss << formatTimestamp(entry.timestamp) << ",";

    // PID
    oss << proc.pid << ",";

    // ProcessName (escaped)
    oss << escapeCSV(wstrToUtf8(proc.name)) << ",";

    // ProcessPath (escaped)
    oss << escapeCSV(wstrToUtf8(proc.path)) << ",";

    // ReadRate (B/s)
    oss << std::setprecision(2) << proc.readRate << ",";

    // WriteRate (B/s)
    oss << proc.writeRate << ",";

    // TotalRate (B/s)
    oss << proc.totalRate() << ",";

    // SessionReadBytes
    oss << proc.sessionReadBytes << ",";

    // SessionWriteBytes
    oss << proc.sessionWriteBytes << ",";

    // SessionTotalBytes
    oss << proc.totalSessionIO() << ",";

    // ProcessReadBytes
    oss << proc.processReadBytes << ",";

    // ProcessWriteBytes
    oss << proc.processWriteBytes << ",";

    // Event
    oss << entry.event << "\n";

    return oss.str();
}

// ── Escape a field for CSV (wrap in quotes if contains comma/quote/newline) ──
std::string Recorder::escapeCSV(const std::string& field) const {
    // Check if escaping is needed
    bool needEscape = false;
    for (char c : field) {
        if (c == ',' || c == '"' || c == '\n' || c == '\r') {
            needEscape = true;
            break;
        }
    }
    if (!needEscape) return field;

    // Escape: wrap in double quotes, double any internal quotes
    std::string escaped = "\"";
    for (char c : field) {
        if (c == '"') escaped += "\"\"";
        else escaped += c;
    }
    escaped += "\"";
    return escaped;
}

// ── Convert wide string to UTF-8 ────────────────────────────────────
std::string Recorder::wstrToUtf8(const std::wstring& wstr) const {
    if (wstr.empty()) return "";

    int len = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                   static_cast<int>(wstr.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";

    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                         static_cast<int>(wstr.size()),
                         &result[0], len, nullptr, nullptr);
    return result;
}

// ── Format timestamp as ISO 8601 ────────────────────────────────────
std::string Recorder::formatTimestamp(const std::chrono::system_clock::time_point& tp) const {
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm;
    localtime_s(&tm, &t);

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        tp.time_since_epoch()) % 1000;

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03lld",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec,
             static_cast<long long>(ms.count()));
    return buf;
}
