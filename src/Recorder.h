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
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <queue>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include "Monitor.h"

// ── CSV column definitions ──────────────────────────────────────────
// Timestamp, PID, ProcessName, ProcessPath,
// ReadRate(B/s), WriteRate(B/s), TotalRate(B/s),
// SessionReadBytes, SessionWriteBytes, SessionTotalBytes,
// ProcessReadBytes, ProcessWriteBytes,
// Event
//
// Event types:
//   "sample"    — periodic snapshot (written each sampling interval)
//   "start"     — recording started
//   "stop"      — recording stopped

struct RecordEntry {
    std::chrono::system_clock::time_point timestamp;
    std::vector<ProcessIOData> processes;
    std::string event;          // "start", "stop", "sample"
};

class Recorder {
public:
    Recorder();
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    // Start recording: automatically creates records/ directory under exe path
    bool start();

    // Stop recording: flushes remaining entries and closes file
    void stop();

    // Check if recording is active
    bool isRecording() const { return m_recording.load(); }

    // Enqueue a snapshot for writing (thread-safe, non-blocking)
    void enqueueSample(const std::vector<ProcessIOData>& processes);

    // Get the current CSV file path (for display)
    std::wstring getFilePath() const { return m_filePath; }

private:
    void writerLoop();
    bool ensureRecordsDir();
    void writeEntry(const RecordEntry& entry);
    std::string formatCSVLine(const RecordEntry& entry, const ProcessIOData& proc, size_t rank) const;
    std::string escapeCSV(const std::string& field) const;
    std::string wstrToUtf8(const std::wstring& wstr) const;
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) const;

    std::atomic<bool> m_recording{false};
    std::atomic<bool> m_stopWriter{false};

    std::wstring m_dirPath;
    std::wstring m_filePath;
    FILE* m_file = nullptr;

    // Thread-safe queue
    std::queue<RecordEntry> m_queue;
    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCV;

    std::thread m_writerThread;
};
