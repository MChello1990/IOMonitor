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
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <cstdint>

// ═══════════════════════════════════════════════════════════════════════
//  SMART Debug Trace — Thread-aware diagnostic logging system
// ═══════════════════════════════════════════════════════════════════════

// Enable/disable debug mode at compile time
#ifndef SMART_DEBUG_ENABLED
#define SMART_DEBUG_ENABLED 1
#endif

// ── Trace categories ─────────────────────────────────────────────────
enum class SmartTraceCategory : uint8_t {
    THREAD_LIFECYCLE = 0,  // Thread creation/destruction/state changes
    DISK_DISCOVERY,         // Disk enumeration & bus type detection
    READER_IOCTL,           // IOCTL calls (ATA/NVMe/SCSI pass-through)
    DATA_REFRESH,           // refreshSmartData() entry/exit
    HEALTH_COMPUTE,         // computeHealth() detailed weights
    RATE_COMPUTE,           // computeRates()
    ATTRIBUTE_PARSE,        // SMART attribute parsing
    WINDOW_EVENT,           // WM_* messages, paint, resize
    OVERLAY_EVENT,          // Overlay window lifecycle
    USER_INTERACTION,       // Button clicks, keyboard input
    ERROR_EXCEPTION,        // Errors and exceptions
    COUNT
};

// ── Single trace entry ───────────────────────────────────────────────

struct SmartTraceEntry {
    uint64_t    sequenceId   = 0;      // Monotonic sequence number
    DWORD       threadId     = 0;      // Windows thread ID
    uint32_t    category     = 0;      // SmartTraceCategory
    std::chrono::steady_clock::time_point timestamp;
    std::string function;              // Function name (e.g., "refreshSmartData")
    std::string message;               // Detail message
    double      durationMs    = 0.0;   // Elapsed ms (for paired begin/end entries)
    bool        isBegin       = false; // Begin marker for duration tracking
    uint64_t    pairId        = 0;     // Links begin/end entries
};

// ── Thread runtime statistics ────────────────────────────────────────

struct SmartThreadStats {
    DWORD       threadId      = 0;
    std::string threadName;
    uint64_t    totalEntries  = 0;
    uint64_t    errorCount    = 0;
    double      totalDurationMs = 0.0; // Sum of all paired durations
    double      maxDurationMs = 0.0;
    std::chrono::steady_clock::time_point lastActivity;
    bool        alive         = false;
};

// ── Trace buffer (thread-safe ring buffer) ───────────────────────────

class SmartTraceBuffer {
public:
    static constexpr size_t MAX_ENTRIES = 8192;

    static SmartTraceBuffer& instance() {
        static SmartTraceBuffer s_inst;
        return s_inst;
    }

    void addEntry(SmartTraceEntry&& entry);
    void addBegin(const char* func, SmartTraceCategory cat, const char* msg);
    void addEnd(const char* func, SmartTraceCategory cat, const char* msg = "");
    void addEvent(const char* func, SmartTraceCategory cat, const char* msg);

    // Snapshot the buffer for rendering (thread-safe copy)
    std::vector<SmartTraceEntry> snapshot() const;
    std::vector<SmartThreadStats> threadStats() const;

    // Clear all entries
    void clear();

    // Get total entry count (wraps around buffer)
    uint64_t totalCount() const { return m_totalCount.load(); }

    // ── File logging ──
    // Start writing all trace entries to a log file.
    // Creates file in the executable directory: IO_SMART_Debug_YYYYMMDD_HHMMSS.log
    bool startFileLogging();

    // Stop file logging and close the file.
    void stopFileLogging();

    // Check if file logging is active.
    bool isFileLogging() const { return m_fileLogging.load(); }

    // Get the current log file path.
    std::wstring getLogFilePath() const { return m_logFilePath; }

private:
    SmartTraceBuffer();
    ~SmartTraceBuffer();
    SmartTraceBuffer(const SmartTraceBuffer&) = delete;
    SmartTraceBuffer& operator=(const SmartTraceBuffer&) = delete;

    void updateThreadStats(const SmartTraceEntry& entry);
    void writeEntryToFile(const SmartTraceEntry& entry);

    mutable std::mutex m_mutex;
    std::vector<SmartTraceEntry> m_entries;  // Ring buffer
    size_t m_writeIndex = 0;
    std::atomic<uint64_t> m_totalCount{0};
    std::atomic<uint64_t> m_seqCounter{0};

    // Per-thread tracking for begin/end pairing
    struct ActiveSpan {
        std::chrono::steady_clock::time_point startTime;
        SmartTraceCategory category;
        uint64_t pairId;
    };
    std::unordered_map<DWORD, std::vector<ActiveSpan>> m_activeSpans;

    // Thread statistics
    std::unordered_map<DWORD, SmartThreadStats> m_threadStats;

    // File logging
    std::atomic<bool> m_fileLogging{false};
    FILE* m_logFile = nullptr;
    std::wstring m_logFilePath;
};

// ── RAII scoped trace helper ─────────────────────────────────────────
// Usage: SMART_TRACE_SCOPE("refreshSmartData", SmartTraceCategory::DATA_REFRESH, "Starting refresh");

#if SMART_DEBUG_ENABLED
    #define SMART_TRACE_BEGIN(func, cat, msg) \
        SmartTraceBuffer::instance().addBegin(func, cat, msg)
    #define SMART_TRACE_END(func, cat, msg) \
        SmartTraceBuffer::instance().addEnd(func, cat, msg)
    #define SMART_TRACE_EVENT(func, cat, msg) \
        SmartTraceBuffer::instance().addEvent(func, cat, msg)

    struct SmartTraceScope {
        const char* func;
        SmartTraceCategory cat;
        SmartTraceScope(const char* f, SmartTraceCategory c, const char* msg)
            : func(f), cat(c) { SMART_TRACE_BEGIN(f, c, msg); }
        ~SmartTraceScope() { SMART_TRACE_END(func, cat, ""); }
    };
    #define SMART_TRACE_SCOPE(func, cat, msg) \
        SmartTraceScope _trace_scope_##__COUNTER__(func, cat, msg)
#else
    #define SMART_TRACE_BEGIN(func, cat, msg)  ((void)0)
    #define SMART_TRACE_END(func, cat, msg)    ((void)0)
    #define SMART_TRACE_EVENT(func, cat, msg)  ((void)0)
    #define SMART_TRACE_SCOPE(func, cat, msg)  ((void)0)
#endif
