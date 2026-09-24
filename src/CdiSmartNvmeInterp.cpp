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

// ═══════════════════════════════════════════════════════════════════════
//  CdiSmartNvmeInterp — NVMe "SMART / Health Information" log to the
//  ATA-style SMART_ATTRIBUTE list CrystalDiskInfo displays.
//
//  UPSTREAM PROVENANCE
//  ───────────────────
//  Ported from the NVMe interpreter that CrystalDiskInfo bundles:
//
//      NVMeInterpreter.cpp / NVMeInterpreter.h
//      Author : Minkyu Kim
//         Web : http://naraeon.net/
//               https://github.com/ebangin127/
//     License : MIT License
//
//  i.e. this interpreter is from <https://github.com/ebangin127/> (MIT),
//  bundled by CrystalDiskInfo <https://github.com/hiyohiyo/CrystalDiskInfo>
//  as NVMeInterpreter.  Upstream folds the whole translation into fifteen
//  near-identical SeperateXxxFrom() helpers, one AddToATASmartBuf() store,
//  and four public entry points; the port keeps that shape.
//
//  SIGNATURE ADAPTATION
//  ───────────────────
//  Upstream is (UCHAR* NVMeSmartBuf, void* ATASmartBufUncasted) and casts
//  the second argument to SMART_ATTRIBUTE_LIST* — an unbounded store into a
//  fixed 30 entry array.  CdiSmart.h instead declares
//
//      DWORD NVMeXxxSmartToATASmart(const BYTE* nvmeSmartBuf,
//                                   SMART_ATTRIBUTE* out, DWORD capacity);
//
//  so every store goes through the bounds-checked AddToATASmartBuf() below
//  and the entry points return the number of attributes actually written
//  (0 on failure).  `out` is NOT zeroed here; the caller (DRIVE_INFO
//  declares Attribute[MAX_ATTRIBUTE]{} , and upstream likewise relies on a
//  zero initialised array) has already done that, and slots that upstream
//  deliberately never writes must stay as the caller left them.
//
//  INDEX / ID LAYOUT  (the generated table in CdiSmartAttributeTable.cpp,
//  kSmartNVMe, ids 0x01..0x1D, was built against exactly this layout)
//
//   write idx | Id | NVMe log byte offset        | bytes copied
//   ----------+----+-----------------------------+-------------
//        0    |  1 |   0  Critical Warning        | 1
//        1    |  2 |   1  Composite Temperature   | 2
//        2    |  3 |   3  Available Spare         | 1
//        3    |  4 |   4  Available Spare Thr.    | 1
//        4    |  5 |   5  Percentage Used         | 1
//        5    |  6 |  32  Data Units Read         | 6
//        6    |  7 |  48  Data Units Written      | 6
//        7    |  8 |  64  Host Read Commands      | 6
//        8    |  9 |  80  Host Write Commands     | 6
//        9    | 10 |  96  Controller Busy Time    | 6
//       10    | 11 | 112  Power Cycles            | 6
//       11    | 12 | 128  Power On Hours          | 6
//       12    | 13 | 144  Unsafe Shutdowns        | 6
//       13    | 14 | 160  Media Errors            | 6
//       14    | 15 | 176  Num Error Info Log Ent. | 6
//       15    | 16 | 192  Warning Composite T. T. | 4   } composite helper
//       16    | 17 | 196  Critical Composite T.T. | 4   } only
//       17    | —  | —                           | —   NEVER WRITTEN
//    18..25   |18..25| 200 + 2*i, i = 0..7       | 2   } sensor helper,
//                                                                skipped when
//                                                                both bytes 0
//    26..29   |26..29| 216 + 4*i, i = 0..3       | 4   } thermal helper,
//                                                                unconditional
//
//  Note the off-by-one in the first two blocks: indices 0..16 carry Ids
//  1..17, because the helpers advance a single cursor that they use both as
//  the write index and (for the sensor/thermal groups) as the Id source.
//  Upstream NVMeCompositeTemperatureSmartToATASmart starts its cursor at 15
//  while its SeperateXxxFrom() helpers hard code Ids 16 and 17, so index 15
//  holds Id 16 and index 16 holds Id 17.  Upstream
//  NVMeTemperatureSensorSmartToATASmart starts its cursor at 17 but does
//  `attr.Id = ++IdxInBuf` BEFORE the store, so its first entry lands on
//  index 18 with Id 18 and index 17 is left untouched by every code path in
//  this file.  The 29 ids then fit the 30 slot array exactly.
//
//  UPSTREAM QUIRKS PRESERVED (each one is load bearing on the layout above)
//  ─────────────────────────────────────────────────────────────────────
//   1. NVMeTemperatureSensorSmartToATASmart writes NOTHING for a sensor
//      whose two raw bytes are both zero.  Its slot stays what the caller
//      initialised it to.  (The later indices still line up: the cursor is
//      bumped every iteration whether or not the store happens.)
//   2. NVMeCompositeTemperatureSmartToATASmart and
//      NVMeThermalManagementTemperatureSmartToATASmart write
//      unconditionally, zero values included.  These two are invoked
//      additively over an already populated attribute array (upstream
//      AtaSmart.cpp calls all three in sequence), so they must be able to
//      overwrite a stale slot.  Do NOT unify the three helpers behind one
//      "write only if nonzero" rule.
//   3. Index 17 is never written by any helper — see the layout note above.
//   4. NVMeSmartToATASmart emits the eight temperature sensors itself
//      (upstream calls NVMeTemperatureSensorSmartToATASmart at its end), so
//      it produces indices 0..14 and 18..25, never 15..17.
//   5. The 64-bit log fields are copied with sizeof(RawValue) == 6 bytes,
//      not 8: the attribute raw field is only 6 bytes wide and the top two
//      bytes of those little endian counters are dropped, exactly as
//      upstream does.  Single byte fields are zero extended, because
//      upstream builds every attribute from a value initialised struct.
// ═══════════════════════════════════════════════════════════════════════

// The SDK headers have to come in ahead of CdiSmart.h, and the ATA macro
// names have to be dropped before it, because <winioctl.h> — which
// CdiSmart.h itself pulls in — #defines exactly the names CdiSmart.h then
// declares as constexpr:
//
//     IDENTIFY_BUFFER_SIZE  READ_ATTRIBUTE_BUFFER_SIZE
//     READ_THRESHOLD_BUFFER_SIZE  SMART_CMD  READ_ATTRIBUTES
//     READ_THRESHOLDS  ENABLE_SMART  DISABLE_SMART  SMART_CYL_LOW
//     SMART_CYL_HI  ID_CMD  SMART_READ_LOG  ATA_FLAGS_*
//
// Without this the frozen header expands to `constexpr BYTE 0xB0 = 0xB0;`
// and every translation unit that includes it dies with C2059 at lines
// 80..101.  CdiSmart.h is not ours to change, so the collision is cleared
// here: the SDK headers are included first (their guards make CdiSmart.h's
// own includes no-ops) and the macros are undefined before it is reached.
// Do not rely on these macros below this point.
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#undef IDENTIFY_BUFFER_SIZE
#undef READ_ATTRIBUTE_BUFFER_SIZE
#undef READ_THRESHOLD_BUFFER_SIZE
#undef SMART_CMD
#undef READ_ATTRIBUTES
#undef READ_THRESHOLDS
#undef ENABLE_SMART
#undef DISABLE_SMART
#undef SMART_STATUS
#undef SMART_CYL_LOW
#undef SMART_CYL_HI
#undef ID_CMD
#undef SMART_READ_LOG
#undef READ_LOG_EXT
#undef ATA_FLAGS_DRDY_REQUIRED
#undef ATA_FLAGS_DATA_IN
#undef ATA_FLAGS_DATA_OUT
#undef ATA_FLAGS_48BIT_COMMAND

#include "CdiSmart.h"

#include <cstring>

namespace cdi {
namespace {

// ── Bounds-checked AddToATASmartBuf ───────────────────────────────────
//
// Upstream: `(*ATASmartBuf)[IdxInBuf] = AttrToAdd;` — an unchecked store
// into a fixed SMART_ATTRIBUTE_LIST.  Ported with the caller's capacity in
// hand, and reporting whether the store happened so the entry points can
// count what they actually wrote.

bool AddToATASmartBuf(SMART_ATTRIBUTE* out, DWORD capacity, DWORD idx,
                      const SMART_ATTRIBUTE& attr) {
    if (idx >= capacity) {
        return false;
    }
    out[idx] = attr;
    return true;
}

// ── SeperateXxxFrom — one attribute per NVMe log field ────────────────
//
// Kept one function per field, as upstream, so each offset below stays
// greppable against NVMeInterpreter.cpp.  Every one of them starts from a
// value initialised SMART_ATTRIBUTE, which is what zero extends a short
// copy across the whole 6 byte raw field.

SMART_ATTRIBUTE SeperateCriticalWarningFrom(const BYTE* nvme) {
    SMART_ATTRIBUTE attr{};
    attr.Id = 1;
    attr.RawValue[0] = nvme[0];
    return attr;
}

SMART_ATTRIBUTE SeperateTemperatureFrom(const BYTE* nvme) {
    const int TemperatureStart = 1;
    SMART_ATTRIBUTE attr{};
    attr.Id = 2;
    attr.RawValue[0] = nvme[TemperatureStart];
    attr.RawValue[1] = nvme[TemperatureStart + 1];
    return attr;
}

SMART_ATTRIBUTE SeperateAvailableSpareFrom(const BYTE* nvme) {
    const int AvailableSpareStart = 3;
    SMART_ATTRIBUTE attr{};
    attr.Id = 3;
    attr.RawValue[0] = nvme[AvailableSpareStart];
    return attr;
}

SMART_ATTRIBUTE SeperateAvailableSpareThresholdFrom(const BYTE* nvme) {
    const int AvailableSpareThresholdStart = 4;
    SMART_ATTRIBUTE attr{};
    attr.Id = 4;
    attr.RawValue[0] = nvme[AvailableSpareThresholdStart];
    return attr;
}

SMART_ATTRIBUTE SeperatePercentageUsedFrom(const BYTE* nvme) {
    const int PercentageUsedStart = 5;
    SMART_ATTRIBUTE attr{};
    attr.Id = 5;
    attr.RawValue[0] = nvme[PercentageUsedStart];
    return attr;
}

SMART_ATTRIBUTE SeperateDataUnitsReadFrom(const BYTE* nvme) {
    const int DataUnitsReadStart = 32;
    SMART_ATTRIBUTE attr{};
    attr.Id = 6;
    ::memcpy(attr.RawValue, &nvme[DataUnitsReadStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateDataUnitsWrittenFrom(const BYTE* nvme) {
    const int DataUnitsWrittenStart = 48;
    SMART_ATTRIBUTE attr{};
    attr.Id = 7;
    ::memcpy(attr.RawValue, &nvme[DataUnitsWrittenStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateHostReadCommandsFrom(const BYTE* nvme) {
    const int ReadStart = 64;
    SMART_ATTRIBUTE attr{};
    attr.Id = 8;
    ::memcpy(attr.RawValue, &nvme[ReadStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateHostWriteCommandsFrom(const BYTE* nvme) {
    const int WriteStart = 80;
    SMART_ATTRIBUTE attr{};
    attr.Id = 9;
    ::memcpy(attr.RawValue, &nvme[WriteStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateControllerBusyTimeFrom(const BYTE* nvme) {
    const int BusyTimeStart = 96;
    SMART_ATTRIBUTE attr{};
    attr.Id = 10;
    ::memcpy(attr.RawValue, &nvme[BusyTimeStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperatePowerCyclesFrom(const BYTE* nvme) {
    const int PowerCycleStart = 112;
    SMART_ATTRIBUTE attr{};
    attr.Id = 11;
    ::memcpy(attr.RawValue, &nvme[PowerCycleStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperatePowerOnHoursFrom(const BYTE* nvme) {
    const int PowerOnHoursStart = 128;
    SMART_ATTRIBUTE attr{};
    attr.Id = 12;
    ::memcpy(attr.RawValue, &nvme[PowerOnHoursStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateUnsafeShutdownsFrom(const BYTE* nvme) {
    const int UnsafeShutdownsStart = 144;
    SMART_ATTRIBUTE attr{};
    attr.Id = 13;
    ::memcpy(attr.RawValue, &nvme[UnsafeShutdownsStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateMediaErrorsFrom(const BYTE* nvme) {
    const int MediaErrorsStart = 160;
    SMART_ATTRIBUTE attr{};
    attr.Id = 14;
    ::memcpy(attr.RawValue, &nvme[MediaErrorsStart], sizeof(attr.RawValue));
    return attr;
}

SMART_ATTRIBUTE SeperateNumberOfErrorsFrom(const BYTE* nvme) {
    const int NumberOfErrorsStart = 176;
    SMART_ATTRIBUTE attr{};
    attr.Id = 15;
    ::memcpy(attr.RawValue, &nvme[NumberOfErrorsStart], sizeof(attr.RawValue));
    return attr;
}

// The two composite temperature counters are 32 bit; upstream copies 4
// bytes, so the port does too (note: RawValue itself is 6 bytes, the other
// two stay zero).
SMART_ATTRIBUTE SeperateWarningCompositeTemperatureTime(const BYTE* nvme) {
    const int TemperatureTimeStart = 192;
    SMART_ATTRIBUTE attr{};
    attr.Id = 16;
    ::memcpy(attr.RawValue, &nvme[TemperatureTimeStart], 4);
    return attr;
}

SMART_ATTRIBUTE SeperateCriticalCompositeTemperatureTime(const BYTE* nvme) {
    const int TemperatureTimeStart = 196;
    SMART_ATTRIBUTE attr{};
    attr.Id = 17;
    ::memcpy(attr.RawValue, &nvme[TemperatureTimeStart], 4);
    return attr;
}

// Clamp the caller's capacity onto the array the layout above is defined
// against.  Never returns more than MAX_ATTRIBUTE.
DWORD ClampCapacity(DWORD capacity) {
    return capacity > static_cast<DWORD>(MAX_ATTRIBUTE)
               ? static_cast<DWORD>(MAX_ATTRIBUTE)
               : capacity;
}

} // namespace

// ── NVMeSmartToATASmart ───────────────────────────────────────────────
//
// The main entry point: the fifteen scalar/64 bit fields (indices 0..14,
// Ids 1..15) followed by the eight temperature sensors (indices 18..25,
// Ids 18..25).  Indices 15..17 are deliberately left to the caller, as
// upstream leaves them.
//
// Returns the number of attributes written; 0 on a null argument.

DWORD NVMeSmartToATASmart(const BYTE* nvmeSmartBuf, SMART_ATTRIBUTE* out,
                          DWORD capacity) {
    if (nvmeSmartBuf == nullptr || out == nullptr) {
        return 0;
    }

    const DWORD cap = ClampCapacity(capacity);
    DWORD written = 0;
    DWORD IdxInBuf = 0;

    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateCriticalWarningFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateTemperatureFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateAvailableSpareFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateAvailableSpareThresholdFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperatePercentageUsedFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateDataUnitsReadFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateDataUnitsWrittenFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateHostReadCommandsFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateHostWriteCommandsFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateControllerBusyTimeFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperatePowerCyclesFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperatePowerOnHoursFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateUnsafeShutdownsFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateMediaErrorsFrom(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateNumberOfErrorsFrom(nvmeSmartBuf))) {
        ++written;
    }

    // Upstream: NVMeTemperatureSensorSmartToATASmart(NVMeSmartBuf,
    // ATASmartBuf);  — it carries its own cursor, starting at 17.
    written += NVMeTemperatureSensorSmartToATASmart(nvmeSmartBuf, out, cap);

    return written;
}

// ── NVMeCompositeTemperatureSmartToATASmart ───────────────────────────
//
// Ids 16 and 17 (indices 15 and 16).  Additive over the main entry point,
// so both stores are unconditional — including on a value of zero.
//
// Returns the number of attributes written; 0 on a null argument.

DWORD NVMeCompositeTemperatureSmartToATASmart(const BYTE* nvmeSmartBuf,
                                              SMART_ATTRIBUTE* out,
                                              DWORD capacity) {
    if (nvmeSmartBuf == nullptr || out == nullptr) {
        return 0;
    }

    const DWORD cap = ClampCapacity(capacity);
    DWORD written = 0;
    DWORD IdxInBuf = 15;

    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateWarningCompositeTemperatureTime(nvmeSmartBuf))) {
        ++written;
    }
    if (AddToATASmartBuf(out, cap, IdxInBuf++,
                         SeperateCriticalCompositeTemperatureTime(nvmeSmartBuf))) {
        ++written;
    }

    return written;
}

// ── NVMeTemperatureSensorSmartToATASmart ──────────────────────────────
//
// Ids 18..25 at indices 18..25, two bytes each from offset 200 + 2*i.
//
// upstream quirk: the cursor is pre-incremented (`attr.Id = ++IdxInBuf`)
// BEFORE the store, so the first sensor lands on index 18 and index 17 is
// never written.  upstream quirk: a sensor whose two raw bytes are both
// zero is skipped entirely — its slot is left as the caller initialised
// it, which is how the caller tells "no sensor" from "sensor at 0 degrees".
// The cursor still advances, so the later sensors keep their indices.
//
// Returns the number of attributes written; 0 on a null argument.

DWORD NVMeTemperatureSensorSmartToATASmart(const BYTE* nvmeSmartBuf,
                                           SMART_ATTRIBUTE* out,
                                           DWORD capacity) {
    if (nvmeSmartBuf == nullptr || out == nullptr) {
        return 0;
    }

    const DWORD cap = ClampCapacity(capacity);
    DWORD written = 0;
    DWORD IdxInBuf = 17;
    const int TemperatureSensorStart = 200;
    const int MaxSensors = 8;

    for (int i = 0; i < MaxSensors; i++) {
        SMART_ATTRIBUTE attr{};
        attr.Id = static_cast<BYTE>(++IdxInBuf);
        ::memcpy(attr.RawValue, &nvmeSmartBuf[TemperatureSensorStart + i * 2], 2);
        // upstream quirk: zero valued sensor -> no store at all, the slot
        // is left as the caller zeroed it.
        if (attr.RawValue[0] != 0 || attr.RawValue[1] != 0) {
            if (AddToATASmartBuf(out, cap, IdxInBuf, attr)) {
                ++written;
            }
        }
    }

    return written;
}

// ── NVMeThermalManagementTemperatureSmartToATASmart ───────────────────
//
// Ids 26..29 at indices 26..29, four bytes each from offset 216 + 4*i.
// Unlike the sensor helper this one stores unconditionally — see quirk 2
// in the banner; upstream calls it over an already populated array.
//
// Returns the number of attributes written; 0 on a null argument.

DWORD NVMeThermalManagementTemperatureSmartToATASmart(const BYTE* nvmeSmartBuf,
                                                      SMART_ATTRIBUTE* out,
                                                      DWORD capacity) {
    if (nvmeSmartBuf == nullptr || out == nullptr) {
        return 0;
    }

    const DWORD cap = ClampCapacity(capacity);
    DWORD written = 0;
    DWORD IdxInBuf = 25;
    const int TemperatureSensorStart = 216;
    const int MaxEntries = 4;

    for (int i = 0; i < MaxEntries; i++) {
        SMART_ATTRIBUTE attr{};
        attr.Id = static_cast<BYTE>(++IdxInBuf);
        ::memcpy(attr.RawValue, &nvmeSmartBuf[TemperatureSensorStart + i * 4], 4);
        if (AddToATASmartBuf(out, cap, IdxInBuf, attr)) {
            ++written;
        }
    }

    return written;
}

} // namespace cdi
