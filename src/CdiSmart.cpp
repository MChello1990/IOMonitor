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
//  CdiSmart — CAtaSmart core: enumeration, identity, refresh
//
//  Ports the following CrystalDiskInfo functions
//  <https://github.com/hiyohiyo/CrystalDiskInfo> (MIT License):
//
//    AtaSmart.cpp:38-66     CAtaSmart::CAtaSmart()
//    AtaSmart.cpp:68-79     CAtaSmart::~CAtaSmart()
//    AtaSmart.cpp:88-353    CAtaSmart::UpdateSmartInfo()
//    AtaSmart.cpp:355-448   CAtaSmart::UpdateIdInfo()
//    AtaSmart.cpp:450-460   CAtaSmart::GetAamValue() / GetApmValue()
//    AtaSmart.cpp:558-652   CAtaSmart::CheckSmartAttributeUpdate()
//    AtaSmart.cpp:2066-2206 CAtaSmart::Init() — the \\\\.\\PhysicalDriveN loop
//    AtaSmart.cpp:2427-2532 CAtaSmart::Init() — the tail (sort)
//    AtaSmart.cpp:2623-3819 CAtaSmart::AddDisk()
//    AtaSmart.cpp:3823-4145 CAtaSmart::AddDiskNVMe()
//    AtaSmart.cpp:12832-41  CAtaSmart::ChangeByteOrder()
//    AtaSmart.cpp:12925-82  CAtaSmart::GetTransferMode()
//    AtaSmart.cpp:12988-90  CAtaSmart::DetectTimeUnit() (upstream inline in AddDisk)
//
//  The other units of the port are:
//    CdiSmartSupport.cpp    shared tables + the FlagLife* side channel
//    CdiSmartIo.cpp         the four acquisition paths (PD / SAT / Si / NVMe)
//    CdiSmartFill.cpp       FillSmartData() / FillSmartThreshold()
//    CdiSmartSsd.cpp        CheckSsdSupport() + the IsSsdXxx classifiers
//    CdiSmartStatus.cpp     CheckDiskStatus() / GetPowerOnHours() / time units
//    CdiSmartNvmeInterp.cpp NVMe SMART log -> ATA-style attribute list
//
//  Deliberate scope limits (mirroring the module banner in CdiSmart.h):
//
//    * no WMI, no INI rules, no drive-letter mapping — the WMI block of
//      upstream Init() (AtaSmart.cpp:821-2064) is skipped entirely, which
//      also removes CMD_TYPE_WMI
//    * no vendor-private NVMe tunnels and no CSMI / MegaRAID / AMD-RC2 /
//      Silicon Image / JMicron-USB-RAID controller paths
//    * DRIVE_INFO carries no *Reverse strings and no Major/Minor, so the
//      model-reversal decision upstream makes in Init() is made inside
//      AddDisk() instead (see the comment there) and the ATA revision
//      string is dropped
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmart.h"
#include "CdiSmartDetail.h"
#include "SmartDebug.h"

#include <algorithm>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cwchar>

namespace cdi {

namespace {

// ── STORAGE_BUS_TYPE values ───────────────────────────────────────────
//
// Spelled numerically rather than with the winioctl.h enumerators so the
// build does not depend on an SDK that names BusTypeSata / BusTypeNvme.
// CrystalDiskInfo carries the same workaround for its Win32 configuration
// (AtaSmart.cpp:2143-2150, "#ifdef _WIN64 ... #else 17 #endif").
constexpr DWORD kBusTypeScsi = 0x01;
constexpr DWORD kBusTypeAta = 0x03;
constexpr DWORD kBusTypeUsb = 0x07;
constexpr DWORD kBusTypeRAID = 0x08;
constexpr DWORD kBusTypeSata = 0x0B;
constexpr DWORD kBusTypeNvme = 0x11;   // 17

// ── Attribute shadow used by CheckSmartAttributeUpdate ────────────────
//
// CrystalDiskInfo keeps a static SMART_ATTRIBUTE attribute[MAX_DISK]
// [MAX_ATTRIBUTE] inside UpdateSmartInfo() and diffs it against the
// current read.  It is stateful: the function copies "cur" into "pre" on
// every successful refresh, so it must be called every time or the next
// diff is computed against a stale baseline.
SMART_ATTRIBUTE g_attributeShadow[MAX_DISK][MAX_ATTRIBUTE] = {};

// ── MFC CString replacements used only by this unit ───────────────────

std::wstring TrimCopy(const char* raw, size_t maxLen) {
    // CString's char* constructor stops at the first NUL; TrimLeft/TrimRight
    // then strip spaces and control characters.
    size_t len = 0;
    while (len < maxLen && raw[len] != '\0') ++len;

    int wideLen = ::MultiByteToWideChar(CP_ACP, 0, raw, static_cast<int>(len), nullptr, 0);
    std::wstring s;
    if (wideLen > 0) {
        s.resize(static_cast<size_t>(wideLen));
        ::MultiByteToWideChar(CP_ACP, 0, raw, static_cast<int>(len), s.data(), wideLen);
    }

    auto isSpace = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
               (c > 0 && c < 0x20);
    };
    size_t b = 0;
    while (b < s.size() && isSpace(s[b])) ++b;
    size_t e = s.size();
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring AnsiToWide(const char* s) {
    if (!s) return std::wstring();
    int len = ::MultiByteToWideChar(CP_ACP, 0, s, -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    ::MultiByteToWideChar(CP_ACP, 0, s, -1, out.data(), len);
    return out;
}

// Reads one of the NUL terminated strings a STORAGE_DEVICE_DESCRIPTOR points
// at.  `offset` is one of its *Offset members, relative to the descriptor.
std::wstring DescriptorString(const STORAGE_DEVICE_DESCRIPTOR* desc, ULONG offset) {
    if (desc == nullptr || offset == 0) return std::wstring();
    return AnsiToWide(reinterpret_cast<const char*>(desc) + offset);
}

// CrystalDiskInfo CAtaSmart::GetModelSerial (AtaSmart.cpp:13182-13198).
std::wstring GetModelSerial(const std::wstring& model, const std::wstring& serial) {
    return model + serial;
}

// CrystalDiskInfo CAtaSmart::GetTransferMode (AtaSmart.cpp:12925-12982).
// Ported verbatim; the CString out-parameters become std::wstring& and the
// INTERFACE_TYPE* keeps its role of overriding the bus type derived from
// the storage descriptor.
DWORD GetTransferMode(WORD w63, WORD w76, WORD w77, WORD w88,
                      std::wstring& current, std::wstring& max, std::wstring& type,
                      INTERFACE_TYPE* interfaceType) {
    using detail::TRANSFER_MODE_UNKNOWN;
    using detail::TRANSFER_MODE_PIO_DMA;
    using detail::TRANSFER_MODE_ULTRA_DMA_16;
    using detail::TRANSFER_MODE_ULTRA_DMA_25;
    using detail::TRANSFER_MODE_ULTRA_DMA_33;
    using detail::TRANSFER_MODE_ULTRA_DMA_44;
    using detail::TRANSFER_MODE_ULTRA_DMA_66;
    using detail::TRANSFER_MODE_ULTRA_DMA_100;
    using detail::TRANSFER_MODE_ULTRA_DMA_133;
    using detail::TRANSFER_MODE_SATA_150;
    using detail::TRANSFER_MODE_SATA_300;
    using detail::TRANSFER_MODE_SATA_600;

    DWORD tm = TRANSFER_MODE_UNKNOWN;
    current.clear();
    max.clear();
    type.clear();
    *interfaceType = INTERFACE_TYPE_UNKNOWN;

    // Multiword DMA or PIO
    if (w63 & 0x0700) {
        tm = TRANSFER_MODE_PIO_DMA;
        current = max = L"PIO/DMA";
    }

    if (w88 & 0x7F) {
        type = L"Parallel ATA";
        *interfaceType = INTERFACE_TYPE_PATA;
    }

    // Ultra DMA Max Transfer Mode
         if (w88 & 0x0040) { tm = TRANSFER_MODE_ULTRA_DMA_133; max = L"UDMA/133"; }
    else if (w88 & 0x0020) { tm = TRANSFER_MODE_ULTRA_DMA_100; max = L"UDMA/100"; }
    else if (w88 & 0x0010) { tm = TRANSFER_MODE_ULTRA_DMA_66;  max = L"UDMA/66";  }
    else if (w88 & 0x0008) { tm = TRANSFER_MODE_ULTRA_DMA_44;  max = L"UDMA/44";  }
    else if (w88 & 0x0004) { tm = TRANSFER_MODE_ULTRA_DMA_33;  max = L"UDMA/33";  }
    else if (w88 & 0x0002) { tm = TRANSFER_MODE_ULTRA_DMA_25;  max = L"UDMA/25";  }
    else if (w88 & 0x0001) { tm = TRANSFER_MODE_ULTRA_DMA_16;  max = L"UDMA/16";  }

    // Ultra DMA Current Transfer Mode
         if (w88 & 0x4000) { current = L"UDMA/133"; }
    else if (w88 & 0x2000) { current = L"UDMA/100"; }
    else if (w88 & 0x1000) { current = L"UDMA/66";  }
    else if (w88 & 0x0800) { current = L"UDMA/44";  }
    else if (w88 & 0x0400) { current = L"UDMA/33";  }
    else if (w88 & 0x0200) { current = L"UDMA/25";  }
    else if (w88 & 0x0100) { current = L"UDMA/16";  }

    // Serial ATA
    if (w76 != 0x0000 && w76 != 0xFFFF) {
        current = max = L"SATA/150";
        type = L"Serial ATA";
        *interfaceType = INTERFACE_TYPE_SATA;
    }

         if (w76 & 0x0010) { tm = TRANSFER_MODE_UNKNOWN; current = max = L"----"; }
    else if (w76 & 0x0008) { tm = TRANSFER_MODE_SATA_600; current = L"----"; max = L"SATA/600"; }
    else if (w76 & 0x0004) { tm = TRANSFER_MODE_SATA_300; current = L"----"; max = L"SATA/300"; }
    else if (w76 & 0x0002) { tm = TRANSFER_MODE_SATA_150; current = L"----"; max = L"SATA/150"; }

    // 2013/5/1 ACS-3
         if (((w77 & 0x000E) >> 1) == 3) { current = L"SATA/600"; }
    else if (((w77 & 0x000E) >> 1) == 2) { current = L"SATA/300"; }
    else if (((w77 & 0x000E) >> 1) == 1) { current = L"SATA/150"; }

    return tm;
}

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  Construction
// ═══════════════════════════════════════════════════════════════════════

CAtaSmart::CAtaSmart() {
    // CrystalDiskInfo gates these on an OS version ladder.  The ladder is
    // flattened here because the port targets Windows 10 / 11 only, where
    // upstream enables all three:
    //
    //   * IOCTL_ATA_PASS_THROUGH has been available since XP SP2
    //   * the NVMe Storage Protocol Specific Query needs Windows 10 1709+,
    //     but no version test is required to be correct: on an OS that does
    //     not implement the property id, DeviceIoControl() fails and the
    //     NVMe branch of DetectDisk() simply falls through to the SATA/SAT
    //     ladder.  Leaving it enabled unconditionally is therefore safe and
    //     avoids the VerifyVersionInfoW manifest trap — an application
    //     without a supportedOS manifest entry is reported as Windows 8 by
    //     that API no matter which OS it really runs on.
    m_bAtaPassThrough = TRUE;
    m_bAtaPassThroughSmart = TRUE;
    m_bNVMeStorageQuery = TRUE;
    m_IsAdvancedDiskSearch = FALSE;

    SMART_TRACE_EVENT("CAtaSmart", SmartTraceCategory::DISK_DISCOVERY,
                      detail::IsWindows10OrGreater()
                          ? "Constructor: ATA pass-through + NVMe storage query enabled"
                          : "Constructor: ATA pass-through + NVMe storage query enabled (pre-Win10 detected)");
}

CAtaSmart::~CAtaSmart() = default;

// ═══════════════════════════════════════════════════════════════════════
//  Enumeration
// ═══════════════════════════════════════════════════════════════════════

BOOL CAtaSmart::Init(BOOL advancedDiskSearch) {
    SMART_TRACE_SCOPE("CAtaSmart::Init", SmartTraceCategory::DISK_DISCOVERY, "Start");

    m_vars.clear();
    detail::ClearLifeCtx();
    m_IsAdvancedDiskSearch = advancedDiskSearch;

    // CrystalDiskInfo iterates \\\\.\\PhysicalDrive0..MAX_SEARCH_PHYSICAL_DRIVE-1.
    // Upstream additionally blacklists drives it has already probed and
    // skips duplicates; both are handled inside DetectDisk() here.
    for (INT i = 0; i < MAX_SEARCH_PHYSICAL_DRIVE; i++) {
        DetectDisk(i);
    }

    // Upstream sorts with qsort + ComparePhysicalDriveId, or by drive letter
    // when flagSortDriveLetter is set.  Drive letters are not tracked in this
    // port, so always sort by PhysicalDriveId.
    std::sort(m_vars.begin(), m_vars.end(),
              [](const DRIVE_INFO& a, const DRIVE_INFO& b) {
                  return a.PhysicalDriveId < b.PhysicalDriveId;
              });

    {
        char summary[64] = {};
        _snprintf_s(summary, _countof(summary), _TRUNCATE, "Complete - %u disk(s)",
                    static_cast<unsigned>(GetDiskCount()));
        SMART_TRACE_EVENT("CAtaSmart::Init", SmartTraceCategory::DISK_DISCOVERY, summary);
    }

    if (GetDiskCount() == 0) {
        // Every acquisition path needs \\\\.\\PhysicalDriveN opened with
        // GENERIC_WRITE, which requires an elevated token.  Surface that so
        // "no disks found" is diagnosable instead of looking like an empty
        // machine.
        SMART_TRACE_EVENT("CAtaSmart::Init", SmartTraceCategory::ERROR_EXCEPTION,
                          "No disk found. Run elevated: reading S.M.A.R.T. requires "
                          "administrator privileges.");
    }

    return GetDiskCount() > 0 ? TRUE : FALSE;
}

BOOL CAtaSmart::DetectDisk(INT physicalDriveId) {
    if (m_vars.size() >= static_cast<size_t>(MAX_DISK)) return FALSE;

    // Overlap check — upstream GetDiskInfo() rejects a drive that is already
    // known (AtaSmart.cpp:6440-6460).
    for (const DRIVE_INFO& known : m_vars) {
        if (known.PhysicalDriveId == physicalDriveId) return FALSE;
    }

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (hIoCtrl == nullptr || hIoCtrl == INVALID_HANDLE_VALUE) return FALSE;

    // The FixedMedia gate is what keeps card readers and USB sticks out of
    // the disk list (AtaSmart.cpp:2100-2108).
    DISK_GEOMETRY dg = {};
    DWORD dwReturned = 0;
    BOOL bRet = ::DeviceIoControl(hIoCtrl, IOCTL_DISK_GET_DRIVE_GEOMETRY,
                                  nullptr, 0, &dg, sizeof(DISK_GEOMETRY),
                                  &dwReturned, nullptr);
    if (bRet == FALSE || dwReturned != sizeof(DISK_GEOMETRY) ||
        dg.MediaType != FixedMedia) {
        ::CloseHandle(hIoCtrl);
        return FALSE;
    }

    // 4096 bytes is enough for STORAGE_DEVICE_DESCRIPTOR plus its strings;
    // the buffer must outlive AddDisk() because "desc" points into it.
    BYTE descBuffer[4096] = {};
    STORAGE_PROPERTY_QUERY sQuery = {};
    sQuery.PropertyId = StorageDeviceProperty;
    sQuery.QueryType = PropertyStandardQuery;

    DWORD dwRet = 0;
    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_STORAGE_QUERY_PROPERTY,
                             &sQuery, sizeof(STORAGE_PROPERTY_QUERY),
                             descBuffer, sizeof(descBuffer), &dwRet, nullptr);
    if (bRet == FALSE) {
        ::CloseHandle(hIoCtrl);
        return FALSE;
    }

    const STORAGE_DEVICE_DESCRIPTOR* desc =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descBuffer);

    std::wstring model;
    std::wstring firmware;
    if (desc->ProductIdOffset) {
        model = AnsiToWide(reinterpret_cast<const char*>(descBuffer) +
                           desc->ProductIdOffset);
    }
    if (desc->ProductRevisionOffset) {
        firmware = AnsiToWide(reinterpret_cast<const char*>(descBuffer) +
                              desc->ProductRevisionOffset);
    }

    INTERFACE_TYPE interfaceType = INTERFACE_TYPE_UNKNOWN;
    if (static_cast<DWORD>(desc->BusType) == kBusTypeNvme) {
        interfaceType = INTERFACE_TYPE_NVME;
    } else if (static_cast<DWORD>(desc->BusType) == kBusTypeUsb) {
        interfaceType = INTERFACE_TYPE_USB;
    }

    ::CloseHandle(hIoCtrl);

    // [2010/12/05] Workaround for SAMSUNG HD155UI / HD204UI
    // http://sourceforge.net/apps/trac/smartmontools/wiki/SamsungF4EGBadBlocks
    // Upstream gates this on its workaroundHD204UI setting; the default there
    // is on, and the check costs three comparisons, so it is kept
    // unconditionally.
    if ((detail::StartsWith(model, L"SAMSUNG HD155UI") ||
         detail::StartsWith(model, L"SAMSUNG HD204UI")) &&
        !detail::StartsWith(firmware, L"1AQ10003")) {
        SMART_TRACE_EVENT("CAtaSmart::DetectDisk", SmartTraceCategory::DISK_DISCOVERY,
                          "Skipped: SAMSUNG HD155UI/HD204UI workaround");
        return FALSE;
    }

    // [2018/10/24] Workaround for FuzeDrive (AMD StoreMi)
    if (detail::Contains(model, L"FuzeDrive") || detail::Contains(model, L"StoreMI")) {
        SMART_TRACE_EVENT("CAtaSmart::DetectDisk", SmartTraceCategory::DISK_DISCOVERY,
                          "Skipped: FuzeDrive/StoreMI virtual volume");
        return FALSE;
    }

    // ── NVMe: Storage Protocol Specific Query ────────────────────────
    if (interfaceType == INTERFACE_TYPE_NVME) {
        if (!m_bNVMeStorageQuery) {
            SMART_TRACE_EVENT("CAtaSmart::DetectDisk", SmartTraceCategory::DISK_DISCOVERY,
                              "NVMe storage query disabled");
            return FALSE;
        }

        IDENTIFY_DEVICE identify = {};
        DWORD diskSize = 0;

        // A failed Identify Controller is not fatal here.  Some Windows 11
        // NVMe driver stacks (the Samsung driver, for one) serve the SMART
        // health log page through this API but return no identify payload;
        // the identity then comes from the storage descriptor instead.  See
        // AddDiskNVMe().  Upstream has no such fallback because the driver
        // generations it targets always answer.
        DoIdentifyDeviceNVMeStorageQuery(physicalDriveId, &identify, &diskSize);

        return AddDiskNVMe(physicalDriveId, &identify, desc, diskSize, L"");
    }

    // ── ATA / SATA / PATA / USB bridge ───────────────────────────────
    //
    // Upstream GetDiskInfo() (AtaSmart.cpp:6490-6520) tries the ATA
    // pass-through identify on target 0xA0 first, wakes the drive and
    // retries, then tries 0xB0.  Only the generic SAT ladder of its large
    // USB vendor switch is kept here (AtaSmart.cpp:6900-6985).
    IDENTIFY_DEVICE identify = {};

    if (!DoIdentifyDevicePd(physicalDriveId, 0xA0, &identify)) {
        detail::WakeUpDisk(physicalDriveId);

        if (!DoIdentifyDevicePd(physicalDriveId, 0xA0, &identify) &&
            !DoIdentifyDevicePd(physicalDriveId, 0xB0, &identify)) {

            if (DoIdentifyDeviceSat(physicalDriveId, 0xA0, &identify, CMD_TYPE_SAT)) {
                // upstream quirk: the target actually used for S.M.A.R.T. is
                // the one that answered IDENTIFY, so it is passed through here
                return AddDisk(physicalDriveId, 0xA0, CMD_TYPE_SAT, &identify,
                               desc, L"");
            }
            if (DoIdentifyDeviceSat(physicalDriveId, 0xB0, &identify, CMD_TYPE_SAT)) {
                return AddDisk(physicalDriveId, 0xB0, CMD_TYPE_SAT, &identify,
                               desc, L"");
            }

            // Legacy DFP_RECEIVE_DRIVE_DATA exchange as the last resort.
            // CdiSmart.h has no COMMAND_TYPE value of its own for this path,
            // so CMD_TYPE_SCSI_MINIPORT marks it — the "legacy fallback"
            // meaning the header comment on GetSmartAttributeSi() documents.
            if (DoIdentifyDeviceSi(physicalDriveId, 0xA0, &identify)) {
                SMART_TRACE_EVENT("CAtaSmart::DetectDisk", SmartTraceCategory::DISK_DISCOVERY,
                                  "Identified via legacy SMART_RCV_DRIVE_DATA");
                return AddDisk(physicalDriveId, 0xA0, CMD_TYPE_SCSI_MINIPORT, &identify,
                               desc, L"");
            }

            SMART_TRACE_EVENT("CAtaSmart::DetectDisk", SmartTraceCategory::DISK_DISCOVERY,
                              "Identify failed on every path");
            return FALSE;
        }
    }

    // upstream quirk: AddDisk() is always called with target 0xA0 for this
    // path, even when the 0xB0 identify is the one that answered.
    return AddDisk(physicalDriveId, 0xA0, CMD_TYPE_PHYSICAL_DRIVE, &identify,
                   desc, L"");
}

// ═══════════════════════════════════════════════════════════════════════
//  Helpers
// ═══════════════════════════════════════════════════════════════════════

// CrystalDiskInfo CAtaSmart::ChangeByteOrder (AtaSmart.cpp:12832-12841).
// ATA identify strings are stored word-swapped.
void ChangeByteOrder(char* str, DWORD length) {
    for (DWORD i = 0; i + 1 < length; i += 2) {
        const char temp = str[i];
        str[i] = str[i + 1];
        str[i + 1] = temp;
    }
}

// CrystalDiskInfo CAtaSmart::DetectTimeUnit — upstream inlines this pair of
// tests in AddDisk() right after GetTimeUnitType() (AtaSmart.cpp:2998-3008).
//
// ATA_SMART_INFO keeps the ATA major revision in a member; DRIVE_INFO does
// not, so it is re-derived from the identify data here.  GetAtaMajorVersion
// is a pure function of word 80, so this is exactly what AddDisk() computed.
void CAtaSmart::DetectTimeUnit(DRIVE_INFO& asi) {
    std::wstring majorVersionString;
    const DWORD major =
        detail::GetAtaMajorVersion(asi.IdentifyDevice.A.MajorVersion, majorVersionString);

    asi.DetectedTimeUnitType =
        detail::GetTimeUnitType(asi.Model, asi.FirmwareRev, major, asi.TransferModeType);

    if (asi.DetectedTimeUnitType == POWER_ON_MILLI_SECONDS) {
        asi.MeasuredTimeUnitType = POWER_ON_MILLI_SECONDS;
    } else if (asi.DetectedTimeUnitType == POWER_ON_10_MINUTES) {
        asi.MeasuredTimeUnitType = POWER_ON_10_MINUTES;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  AddDisk — CrystalDiskInfo AtaSmart.cpp:2623-3819 (trimmed)
// ═══════════════════════════════════════════════════════════════════════

BOOL CAtaSmart::AddDisk(INT physicalDriveId, BYTE target, COMMAND_TYPE commandType,
                        IDENTIFY_DEVICE* identify, const STORAGE_DEVICE_DESCRIPTOR* desc,
                        const std::wstring& pnpDeviceId) {
    if (m_vars.size() >= static_cast<size_t>(MAX_DISK)) return FALSE;
    if (identify == nullptr) return FALSE;

    DRIVE_INFO asi = {};
    DRIVE_INFO asiCheck = {};

    memcpy(&asi.IdentifyDevice, identify, sizeof(ATA_IDENTIFY_DEVICE));
    asi.PhysicalDriveId = physicalDriveId;
    asi.ScsiBus = 0;
    asi.ScsiPort = 0;
    asi.ScsiTargetId = -1;   // upstream Init() passes -1/-1 for the PD loop
    asi.CommandType = commandType;
    asiCheck.CommandType = commandType;
    asi.Target = target;
    asi.PnpDeviceId = pnpDeviceId;

    const UINT commandTypeIndex = static_cast<UINT>(commandType);
    if (commandTypeIndex < 8) {
        asi.CommandTypeString = detail::kCommandTypeString[commandTypeIndex];
        // upstream quirk: for the physical-drive and SAT families the string
        // is suffixed with the target number ("pd1" / "sa2").
        if (commandType == CMD_TYPE_PHYSICAL_DRIVE || commandType == CMD_TYPE_SAT ||
            commandType == CMD_TYPE_JMICRON) {
            asi.CommandTypeString += (target == 0xB0) ? L"2" : L"1";
        }
    }

    // ── Field initialisation (AtaSmart.cpp:2680-2790) ────────────────
    // Members that DRIVE_INFO does not carry are local flags here.
    BOOL isIdInfoIncorrect = FALSE;
    BOOL isNvCacheSupported = FALSE;
    BOOL is9126MB = FALSE;
    BOOL isMaxtorMinute = FALSE;

    asi.IsSmartEnabled = FALSE;
    asi.IsSmartCorrect = FALSE;
    asi.IsThresholdCorrect = FALSE;
    asi.IsWord88 = FALSE;
    asi.IsCheckSumError = FALSE;
    asi.IsThresholdBug = FALSE;
    asi.IsSmartSupported = FALSE;
    asi.IsLba48Supported = FALSE;
    asi.IsNvmeThresholdSupported = FALSE;
    asi.IsNvmeThermalManagementSupported = FALSE;
    asi.IsSsd = FALSE;
    asi.IsTrimSupported = FALSE;
    asi.IsNVMe = FALSE;
    asi.IsUasp = FALSE;

    asi.TotalDiskSize = 0;
    asi.Cylinder = 0;
    asi.Head = 0;
    asi.Sector = 0;
    asi.Sector28 = 0;
    asi.Sector48 = 0;
    asi.NumberOfSectors = 0;
    asi.DiskSizeChs = 0;
    asi.DiskSizeLba28 = 0;
    asi.DiskSizeLba48 = 0;
    asi.LogicalSectorSize = 512;
    asi.PhysicalSectorSize = 512;
    asi.BufferSize = 0;
    asi.NvCacheSize = 0;
    asi.TransferModeType = 0;
    asi.DetectedTimeUnitType = POWER_ON_UNKNOWN;
    asi.MeasuredTimeUnitType = POWER_ON_UNKNOWN;
    asi.AttributeCount = 0;
    asi.PowerOnRawValue = -1;
    asi.PowerOnCount = 0;
    asi.PowerOnHours = -1;
    asi.Temperature = -1000;
    asi.TemperatureMultiplier = 1.0;
    asi.NominalMediaRotationRate = 0;
    asi.Life = -1;
    asi.HostWrites = -1;
    asi.HostReads = -1;
    asi.GBytesErased = -1;
    asi.NandWrites = -1;
    asi.WearLevelingCount = -1;
    asi.HostWritesBytes = 0;
    asi.HostReadsBytes = 0;
    asi.DiskStatus = DISK_STATUS_UNKNOWN;
    asi.InterfaceType = INTERFACE_TYPE_UNKNOWN;
    asi.HostReadsWritesUnit = HOST_READS_WRITES_UNKNOWN;
    asi.DiskVendorId = VENDOR_UNKNOWN;
    asi.UsbVendorId = VENDOR_UNKNOWN;
    asi.UsbProductId = 0;

    // CrystalDiskInfo reads these four from its INI (DiskInfoDlgInit.cpp:441-444)
    // where the documented defaults are 1/1/1/10.  INI rules are out of scope
    // for this port, so the defaults are applied directly.  Leaving them at
    // DRIVE_INFO's 0 would silently disable the CAUTION reports in
    // CheckDiskStatus() for exactly the drives that need them.
    asi.Threshold05 = 1;
    asi.ThresholdC5 = 1;
    asi.ThresholdC6 = 1;
    asi.ThresholdFF = 10;

    // ── Identify integrity ───────────────────────────────────────────
    // Checksum is informational only — cheap USB bridges fail it and still
    // return usable data, so nothing rejects on it.
    {
        BYTE sum = 0;
        BYTE checkSum[IDENTIFY_BUFFER_SIZE] = {};
        memcpy(checkSum, identify, IDENTIFY_BUFFER_SIZE);
        for (int j = 0; j < IDENTIFY_BUFFER_SIZE; j++) sum += checkSum[j];
        if (sum != 0) asi.IsCheckSumError = TRUE;
    }

    // CheckAsciiStringError mutates: control characters become spaces.
    if (detail::CheckAsciiStringError(identify->A.SerialNumber, sizeof(identify->A.SerialNumber)) ||
        detail::CheckAsciiStringError(identify->A.FirmwareRev, sizeof(identify->A.FirmwareRev)) ||
        detail::CheckAsciiStringError(identify->A.Model, sizeof(identify->A.Model))) {
        isIdInfoIncorrect = TRUE;
        return FALSE;
    }

    // ── Strings: the raw (word-swapped) forms are kept first ──────────
    const std::wstring serialNumberReverse =
        TrimCopy(identify->A.SerialNumber, sizeof(identify->A.SerialNumber));
    const std::wstring firmwareRevReverse =
        TrimCopy(identify->A.FirmwareRev, sizeof(identify->A.FirmwareRev));
    const std::wstring modelReverse =
        TrimCopy(identify->A.Model, sizeof(identify->A.Model));

    ChangeByteOrder(identify->A.SerialNumber, sizeof(identify->A.SerialNumber));
    ChangeByteOrder(identify->A.FirmwareRev, sizeof(identify->A.FirmwareRev));
    ChangeByteOrder(identify->A.Model, sizeof(identify->A.Model));

    asi.SerialNumber = TrimCopy(identify->A.SerialNumber, sizeof(identify->A.SerialNumber));
    asi.FirmwareRev = TrimCopy(identify->A.FirmwareRev, sizeof(identify->A.FirmwareRev));
    asi.Model = TrimCopy(identify->A.Model, sizeof(identify->A.Model));

    if (asi.Model.empty() || asi.FirmwareRev.empty()) {
        SMART_TRACE_EVENT("CAtaSmart::AddDisk", SmartTraceCategory::DISK_DISCOVERY,
                          "Model or firmware revision empty after byte order fix-up");
        isIdInfoIncorrect = TRUE;
        return FALSE;
    }

    // ── Model reversal ───────────────────────────────────────────────
    //
    // CrystalDiskInfo decides this in Init() *after* AddDisk() returns, by
    // which point it still has ATA_SMART_INFO::ModelReverse & friends to
    // fall back on (AtaSmart.cpp:2172-2192).  DRIVE_INFO keeps no *Reverse
    // members, so the decision is made here, where the pre-reversal strings
    // are still in scope.
    //
    // The leading substrings are byte-swapped forms of "WDC ", "Hita",
    // "SAMS", "Maxt", "TOSH" and "FUJI": drives that already report
    // little-endian strings get corrupted by ChangeByteOrder, and this
    // branch un-corrupts them.  Skipping it leaves every WDC / HGST /
    // Toshiba / Fujitsu / Samsung drive with a garbage model string.
    if (detail::StartsWith(asi.Model, L"DW C") ||   // WDC
        detail::StartsWith(asi.Model, L"iHat") ||   // Hitachi
        detail::StartsWith(asi.Model, L"ASSM") ||   // SAMSUNG
        detail::StartsWith(asi.Model, L"aMtx") ||   // Maxtor
        detail::StartsWith(asi.Model, L"OTHS") ||   // TOSHIBA
        detail::StartsWith(asi.Model, L"UFIJ")) {   // FUJITSU
        SMART_TRACE_EVENT("CAtaSmart::AddDisk", SmartTraceCategory::DISK_DISCOVERY,
                          "Identify strings were already little endian - using raw forms");
        asi.SerialNumber = serialNumberReverse;
        asi.FirmwareRev = firmwareRevReverse;
        asi.Model = modelReverse;
    }

    // ── Duplicate detection ──────────────────────────────────────────
    // Upstream additionally merges CSMI duplicates here; that path is out of
    // scope, so a duplicate is simply rejected.
    for (const DRIVE_INFO& known : m_vars) {
        if (asi.Model == known.Model && asi.SerialNumber == known.SerialNumber) {
            return FALSE;
        }
        if (modelReverse == known.Model && serialNumberReverse == known.SerialNumber) {
            return FALSE;
        }
    }

    // ADATA SSD firmware 346 reports temperature at a 0.5 multiplier
    // (AtaSmart.cpp:2892-2899).
    {
        std::wstring firmwareRevInt = asi.FirmwareRev;
        firmwareRevInt.erase(std::remove(firmwareRevInt.begin(), firmwareRevInt.end(), L'.'),
                             firmwareRevInt.end());
        if (detail::StartsWith(asi.Model, L"ADATA SSD") && _wtoi(firmwareRevInt.c_str()) == 346) {
            asi.TemperatureMultiplier = 0.5;
        }
    }

    // ── ATA revision and transfer mode ───────────────────────────────
    std::wstring majorVersionString;
    const DWORD major =
        detail::GetAtaMajorVersion(identify->A.MajorVersion, majorVersionString);

    std::wstring currentTransferMode;
    std::wstring maxTransferMode;
    std::wstring interfaceString;
    INTERFACE_TYPE interfaceType = asi.InterfaceType;
    asi.TransferModeType =
        GetTransferMode(identify->A.MultiWordDma, identify->A.SerialAtaCapabilities,
                        identify->A.SerialAtaAdditionalCapabilities, identify->A.UltraDmaMode,
                        currentTransferMode, maxTransferMode, interfaceString, &interfaceType);
    asi.Interface = interfaceString;
    // ATA_SMART_INFO has CurrentTransferMode and MaxTransferMode; DRIVE_INFO
    // has one TransferMode slot, so the informative maximum wins and the
    // current speed is the fallback for the modes that only fill "current".
    asi.TransferMode = maxTransferMode.empty() ? currentTransferMode : maxTransferMode;

    // The storage descriptor outranks the identify words for USB and NVMe
    // (upstream applies it in Init() after AddDisk()).
    SetInterfaceType(asi, desc);

    asi.DetectedTimeUnitType =
        detail::GetTimeUnitType(asi.Model, asi.FirmwareRev, major, asi.TransferModeType);
    if (asi.DetectedTimeUnitType == POWER_ON_MILLI_SECONDS) {
        asi.MeasuredTimeUnitType = POWER_ON_MILLI_SECONDS;
    } else if (asi.DetectedTimeUnitType == POWER_ON_10_MINUTES) {
        asi.MeasuredTimeUnitType = POWER_ON_10_MINUTES;
    }

    // ── Feature set detection (AtaSmart.cpp:3010-3100) ───────────────
    if (major >= 3 && (identify->A.CommandSetSupported1 & (1 << 0))) {
        asi.IsSmartSupported = TRUE;
    }
    if (major >= 5 && (identify->A.CommandSetSupported2 & (1 << 10))) {
        asi.IsLba48Supported = TRUE;
    }
    if (major >= 7 && (identify->A.NvCacheCapabilities & (1 << 0))) {
        isNvCacheSupported = TRUE;
    }
    if (major >= 7 && (identify->A.DataSetManagement & (1 << 0))) {
        asi.IsTrimSupported = TRUE;
    }

    // http://ascii.jp/elem/000/000/203/203345/img.html
    // "NominalMediaRotationRate" is ATA8-ACS but some ATA/ATAPI-7 devices
    // support the field.
    if (major >= 7 && identify->A.NominalMediaRotationRate == 0x01) {
        asi.IsSsd = TRUE;
        asi.NominalMediaRotationRate = 1;
    }
    if (major >= 7 && identify->A.NominalMediaRotationRate >= 0x401 &&
        identify->A.NominalMediaRotationRate < 0xFFFF) {
        asi.NominalMediaRotationRate = identify->A.NominalMediaRotationRate;
    }
    if (major >= 7 && (identify->A.DeviceNominalFormFactor & 0xF) > 0 &&
        (identify->A.DeviceNominalFormFactor & 0xF) <= 5) {
        asi.DeviceNominalFormFactor = detail::DeviceFormFactorString(
            static_cast<WORD>(identify->A.DeviceNominalFormFactor));
    }

    {
        const std::wstring modelUpper = detail::ToUpperCopy(asi.Model);
        if (detail::StartsWith(modelUpper, L"MAXTOR") &&
            asi.DetectedTimeUnitType == POWER_ON_MINUTES) {
            isMaxtorMinute = TRUE;
        }
    }

    // ── Geometry (AtaSmart.cpp:3100-3245) ────────────────────────────
    // Clamping happens BEFORE the values are stored, so the clamped
    // figures are what gets reported.
    if (identify->A.LogicalCylinders > 16383) {
        identify->A.LogicalCylinders = 16383;
        isIdInfoIncorrect = TRUE;
    }
    if (identify->A.LogicalHeads > 16) {
        identify->A.LogicalHeads = 16;
        isIdInfoIncorrect = TRUE;
    }
    if (identify->A.LogicalSectors > 63) {
        identify->A.LogicalSectors = 63;
        isIdInfoIncorrect = TRUE;
    }

    asi.Cylinder = identify->A.LogicalCylinders;
    asi.Head = identify->A.LogicalHeads;
    asi.Sector = identify->A.LogicalSectors;
    asi.Sector28 = 0x0FFFFFFF & identify->A.TotalAddressableSectors;
    asi.Sector48 = 0x0000FFFFFFFFFFFFULL & identify->A.MaxUserLba;

    if ((identify->A.SectorSize & 0xC000) == 0x4000) {   // bit15=0, bit14=1
        if ((identify->A.SectorSize & 0x000F) == 0x3) {  // bit0-3
            asi.LogicalSectorSize = 512;
            asi.PhysicalSectorSize = 4096;
        } else if ((identify->A.SectorSize & 0x1000) == 0x1000) {  // bit12=1
            if (identify->A.WordsPerLogicalSector == 256 ||
                identify->A.WordsPerLogicalSector == 0) {
                asi.LogicalSectorSize = 512;
            } else {
                asi.LogicalSectorSize = identify->A.WordsPerLogicalSector * 2;
            }
        }
    }

    if (asi.PhysicalSectorSize < asi.LogicalSectorSize) {
        asi.PhysicalSectorSize = asi.LogicalSectorSize;
    }

    if (identify->A.TotalAddressableSectors == 0x01100003) {  // 9126807040 bytes
        is9126MB = TRUE;
    }

    if (identify->A.LogicalCylinders == 0 || identify->A.LogicalHeads == 0 ||
        identify->A.LogicalSectors == 0) {
        // Realtek RTL9210 support (2024/01/19): a bridge that reports a
        // completely empty geometry through SAT has no disk behind it.
        if (identify->A.Capabilities1 == 0 && identify->A.Capabilities2 == 0 &&
            commandType == CMD_TYPE_SAT) {
            return FALSE;
        }
        asi.DiskSizeChs = 0;
    } else if (((ULONGLONG)identify->A.LogicalCylinders * identify->A.LogicalHeads *
                identify->A.LogicalSectors * 512) / 1000 / 1000 > 1000) {
        asi.DiskSizeChs = (DWORD)(((ULONGLONG)identify->A.LogicalCylinders *
                                   identify->A.LogicalHeads * identify->A.LogicalSectors * 512) /
                                  1000 / 1000 - 49);
    } else {
        asi.DiskSizeChs = (DWORD)(((ULONGLONG)identify->A.LogicalCylinders *
                                   identify->A.LogicalHeads * identify->A.LogicalSectors * 512) /
                                  1000 / 1000);
    }

    asi.NumberOfSectors = (ULONGLONG)identify->A.LogicalCylinders *
                          identify->A.LogicalHeads * identify->A.LogicalSectors;
    if (asi.Sector28 > 0 && ((ULONGLONG)asi.Sector28 * 512) / 1000 / 1000 > 49) {
        asi.DiskSizeLba28 = (DWORD)(((ULONGLONG)asi.Sector28 * 512) / 1000 / 1000 - 49);
        asi.NumberOfSectors = asi.Sector28;
    } else {
        asi.DiskSizeLba28 = 0;
    }

    if (asi.IsLba48Supported &&
        (asi.Sector48 * asi.LogicalSectorSize) / 1000 / 1000 > 49) {
        asi.DiskSizeLba48 = (DWORD)((asi.Sector48 * asi.LogicalSectorSize) / 1000 / 1000 - 49);
        asi.NumberOfSectors = asi.Sector48;
    } else {
        asi.DiskSizeLba48 = 0;
    }

    asi.BufferSize = identify->A.BufferSize * 512;
    if (isNvCacheSupported) {
        asi.NvCacheSize = (ULONGLONG)identify->A.NvCacheSizeLogicalBlocks * 512;
    }

    // upstream quirk: a drive reporting more than 28 bits in word 60-61 has
    // an untrustworthy figure there, so TotalDiskSize is zeroed rather than
    // taking the maximum.
    if (identify->A.TotalAddressableSectors > 0x0FFFFFFF) {
        asi.TotalDiskSize = 0;
    } else if (asi.DiskSizeLba48 > asi.DiskSizeLba28) {
        asi.TotalDiskSize = asi.DiskSizeLba48;
    } else if (asi.DiskSizeLba28 > asi.DiskSizeChs) {
        asi.TotalDiskSize = asi.DiskSizeLba28;
    } else {
        asi.TotalDiskSize = asi.DiskSizeChs;
    }

    // Error check for an external ATA controller.  Note upstream clears
    // DiskSizeLba48 but leaves NumberOfSectors (already overwritten from
    // Sector48) alone — preserved as-is.
    if (asi.IsLba48Supported && identify->A.TotalAddressableSectors < 268435455 &&
        asi.DiskSizeLba28 != asi.DiskSizeLba48) {
        asi.DiskSizeLba48 = 0;
    }

    // ── S.M.A.R.T. read (AtaSmart.cpp:3246-3700) ─────────────────────
    if (asi.IsSmartSupported || is9126MB || m_IsAdvancedDiskSearch) {
        BOOL (*readSmart)(CAtaSmart&, DRIVE_INFO&) = nullptr;
        BOOL (*readThreshold)(CAtaSmart&, DRIVE_INFO&) = nullptr;
        BOOL (*enableSmart)(CAtaSmart&, const DRIVE_INFO&) = nullptr;

        switch (asi.CommandType) {
        case CMD_TYPE_PHYSICAL_DRIVE:
            readSmart = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartAttributePd(a.PhysicalDriveId, a.Target, &a);
            };
            readThreshold = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartThresholdPd(a.PhysicalDriveId, a.Target, &a);
            };
            enableSmart = [](CAtaSmart& self, const DRIVE_INFO& a) {
                return self.ControlSmartStatusPd(a.PhysicalDriveId, a.Target, ENABLE_SMART);
            };
            break;

        case CMD_TYPE_SAT:
        case CMD_TYPE_JMICRON:
            readSmart = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartAttributeSat(a.PhysicalDriveId, a.Target, &a);
            };
            readThreshold = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartThresholdSat(a.PhysicalDriveId, a.Target, &a);
            };
            enableSmart = [](CAtaSmart& self, const DRIVE_INFO& a) {
                return self.ControlSmartStatusSat(a.PhysicalDriveId, a.Target, ENABLE_SMART,
                                                  a.CommandType);
            };
            break;

        case CMD_TYPE_SCSI_MINIPORT:
            // The legacy DFP_RECEIVE_DRIVE_DATA fallback; see DetectDisk().
            readSmart = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartAttributeSi(a.PhysicalDriveId, &a);
            };
            readThreshold = [](CAtaSmart& self, DRIVE_INFO& a) {
                return self.GetSmartThresholdSi(a.PhysicalDriveId, &a);
            };
            break;

        default:
            break;
        }

        if (readSmart != nullptr) {
            if (readSmart(*this, asi)) {
                CheckSsdSupport(asi);
                readSmart(*this, asiCheck);
                if (detail::CheckSmartAttributeCorrect(&asi, &asiCheck)) {
                    asi.IsSmartCorrect = TRUE;
                }
                if (readThreshold != nullptr && readThreshold(*this, asi)) {
                    asi.IsThresholdCorrect = TRUE;
                }
                asi.IsSmartEnabled = TRUE;
            }

            if (!asi.IsSmartCorrect && enableSmart != nullptr && enableSmart(*this, asi)) {
                if (readSmart(*this, asi)) {
                    CheckSsdSupport(asi);
                    readSmart(*this, asiCheck);
                    if (detail::CheckSmartAttributeCorrect(&asi, &asiCheck)) {
                        asi.IsSmartCorrect = TRUE;
                    }
                    if (readThreshold != nullptr && readThreshold(*this, asi)) {
                        asi.IsThresholdCorrect = TRUE;
                    }
                    asi.IsSmartEnabled = TRUE;
                }
            }

            // 2012/9/12 - https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=821;id=diskinfo#821
            // 2013/12/2 - https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1330;id=diskinfo#1330
            // A bridge that answers READ ATTRIBUTES with the thresholds page
            // is not returning usable data.
            if (memcmp(asi.SmartReadData, asi.SmartReadThreshold, 512) == 0 &&
                asi.DiskVendorId != SSD_VENDOR_INDILINX) {
                SMART_TRACE_EVENT("CAtaSmart::AddDisk", SmartTraceCategory::ATTRIBUTE_PARSE,
                                  "SmartReadData == SmartReadThreshold - discarding");
                asi.IsSmartCorrect = FALSE;
                asi.IsThresholdCorrect = FALSE;
                asi.IsSmartEnabled = FALSE;
            }
        }
    }

    // ── Threshold bugs (AtaSmart.cpp:3741-3756) ──────────────────────
    // Only computed after CheckSsdSupport() has assigned the vendor.
    if (asi.DiskVendorId == SSD_VENDOR_SANDFORCE &&
        ((detail::StartsWith(asi.Model, L"OCZ-VERTEX3") &&
          detail::StartsWith(asi.FirmwareRev, L"2.02")) ||
         (detail::StartsWith(asi.Model, L"OCZ-VERTEX2") &&
          detail::StartsWith(asi.FirmwareRev, L"1.27")))) {
        asi.IsThresholdBug = TRUE;
    } else if (detail::StartsWith(asi.Model, L"SSD G2 Series") &&
               detail::StartsWith(asi.FirmwareRev, L"3.6.5")) {
        asi.IsThresholdBug = TRUE;
    }

    if (!asi.IsSmartCorrect) {
        asi.PowerOnHours = -1;
        asi.PowerOnRawValue = -1;
        asi.PowerOnCount = 0;
        asi.Temperature = -1000;
        asi.DiskStatus = DISK_STATUS_UNKNOWN;
    }

    // Workaround for the Intel SSD power-on-hours offset (AtaSmart.cpp:3772-3780)
    if (detail::StartsWith(asi.Model, L"Intel") && asi.PowerOnHours > 0x0DA753) {
        asi.PowerOnRawValue -= 0x0DA753;
        asi.PowerOnHours -= 0x0DA753;
    }

    // upstream quirk: the literal below is what real JMicron RAID volumes
    // report (a lowercase L, not "RAID").
    if (detail::StartsWith(asi.Model, L"JMicron RAlD")) {
        return FALSE;
    }

    if (isIdInfoIncorrect && (!m_IsAdvancedDiskSearch || commandType >= CMD_TYPE_SAT)) {
        return FALSE;
    }

    m_vars.push_back(asi);
    return TRUE;
}

// ═══════════════════════════════════════════════════════════════════════
//  AddDiskNVMe — CrystalDiskInfo AtaSmart.cpp:3823-4145 (trimmed)
// ═══════════════════════════════════════════════════════════════════════

BOOL CAtaSmart::AddDiskNVMe(INT physicalDriveId, IDENTIFY_DEVICE* identify,
                            const STORAGE_DEVICE_DESCRIPTOR* desc, DWORD diskSize,
                            const std::wstring& pnpDeviceId) {
    if (m_vars.size() >= static_cast<size_t>(MAX_DISK)) return FALSE;
    if (identify == nullptr) return FALSE;

    DRIVE_INFO asi = {};

    // The full 4096 byte controller buffer is copied: NVMe identify
    // attributes live beyond the 512 byte ATA window (Bin[520], Bin[525]).
    memcpy(&asi.IdentifyDevice, identify, sizeof(NVME_IDENTIFY_DEVICE));
    asi.PhysicalDriveId = physicalDriveId;
    asi.ScsiBus = 0;
    asi.ScsiPort = 0;
    asi.ScsiTargetId = -1;
    asi.CommandType = CMD_TYPE_NVME_STORAGE_QUERY;
    asi.PnpDeviceId = pnpDeviceId;
    asi.Target = static_cast<BYTE>(physicalDriveId);

    const UINT commandTypeIndex = static_cast<UINT>(asi.CommandType);
    if (commandTypeIndex < 8) {
        asi.CommandTypeString = detail::kCommandTypeString[commandTypeIndex];
    }

    asi.IsSmartEnabled = TRUE;
    asi.IsSmartCorrect = TRUE;
    asi.IsThresholdCorrect = TRUE;
    asi.IsSmartSupported = TRUE;
    asi.IsSsd = TRUE;
    asi.IsNVMe = TRUE;
    asi.IsNvmeThresholdSupported = FALSE;
    asi.IsNvmeThermalManagementSupported = FALSE;
    asi.IsTrimSupported = FALSE;

    asi.TotalDiskSize = 0;
    asi.LogicalSectorSize = 512;
    asi.PhysicalSectorSize = 512;
    asi.TransferModeType = 0;
    asi.DetectedTimeUnitType = POWER_ON_UNKNOWN;
    asi.MeasuredTimeUnitType = POWER_ON_UNKNOWN;
    asi.AttributeCount = 0;
    asi.PowerOnRawValue = -1;
    asi.PowerOnCount = 0;
    asi.PowerOnHours = -1;
    asi.Temperature = -1000;
    asi.TemperatureMultiplier = 1.0;
    asi.NominalMediaRotationRate = 1;
    asi.Life = -1;
    asi.HostWrites = -1;
    asi.HostReads = -1;
    asi.GBytesErased = -1;
    asi.NandWrites = -1;
    asi.WearLevelingCount = -1;
    asi.HostWritesBytes = 0;
    asi.HostReadsBytes = 0;
    asi.DiskStatus = DISK_STATUS_UNKNOWN;
    asi.InterfaceType = INTERFACE_TYPE_NVME;
    asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
    asi.DiskVendorId = VENDOR_UNKNOWN;
    asi.UsbVendorId = VENDOR_UNKNOWN;
    asi.UsbProductId = 0;

    // Identity: Identify Controller first, exactly as CrystalDiskInfo does.
    // When the driver returns no identify payload (see DetectDisk), fall back
    // to the strings the storage stack reports in its device descriptor —
    // without this the drive would be dropped entirely, losing not just its
    // name but its whole S.M.A.R.T. page.
    asi.Model = TrimCopy(asi.IdentifyDevice.N.Model, sizeof(asi.IdentifyDevice.N.Model));
    if (!asi.Model.empty()) {
        asi.SerialNumber =
            TrimCopy(asi.IdentifyDevice.N.SerialNumber, sizeof(asi.IdentifyDevice.N.SerialNumber));
        asi.FirmwareRev =
            TrimCopy(asi.IdentifyDevice.N.FirmwareRev, sizeof(asi.IdentifyDevice.N.FirmwareRev));
    } else {
        asi.Model = DescriptorString(desc, desc ? desc->ProductIdOffset : 0);
        asi.FirmwareRev = DescriptorString(desc, desc ? desc->ProductRevisionOffset : 0);
        asi.SerialNumber = DescriptorString(desc, desc ? desc->SerialNumberOffset : 0);

        if (!asi.Model.empty()) {
            SMART_TRACE_EVENT("CAtaSmart::AddDiskNVMe", SmartTraceCategory::DISK_DISCOVERY,
                              "Identify Controller payload unavailable - identity taken "
                              "from the storage descriptor");
        }
    }

    if (asi.Model.empty()) {
        SMART_TRACE_EVENT("CAtaSmart::AddDiskNVMe", SmartTraceCategory::DISK_DISCOVERY,
                          "No identity from Identify Controller or storage descriptor");
        return FALSE;
    }

    if (diskSize != 0) {
        asi.TotalDiskSize = diskSize;
    }

    if (asi.IdentifyDevice.B.Bin[520] & 0x4) {   // Dataset Management support
        asi.IsTrimSupported = TRUE;
    }

    // Duplicate detection — the JMS586 branch of upstream is out of scope.
    for (const DRIVE_INFO& known : m_vars) {
        if (asi.Model == known.Model && asi.SerialNumber == known.SerialNumber) {
            return FALSE;
        }
    }

    SetInterfaceType(asi, desc);

    if (m_bNVMeStorageQuery &&
        GetSmartAttributeNVMeStorageQuery(physicalDriveId, &asi)) {
        asi.IsSmartSupported = TRUE;

        asi.Temperature = asi.SmartReadData[0x2] * 256 + asi.SmartReadData[0x1] - 273;
        // upstream quirk: AddDiskNVMe() rejects above 100 C while
        // UpdateSmartInfo() rejects above 200 C.  Both are preserved.
        if (asi.Temperature == -273 || asi.Temperature > 100) {
            asi.Temperature = -1000;
        }

        asi.Life = 100 - asi.SmartReadData[0x05];
        if (asi.Life < 0) {
            asi.Life = 0;
        }

        asi.HostReads =
            static_cast<INT>((detail::B8toB64lePtr(&asi.SmartReadData[0x20]) * 1000) >> 21);
        asi.HostWrites =
            static_cast<INT>((detail::B8toB64lePtr(&asi.SmartReadData[0x30]) * 1000) >> 21);
        asi.PowerOnCount = detail::B8toB32lePtr(&asi.SmartReadData[0x70]);
        asi.PowerOnHours = detail::B8toINTlePtr(&asi.SmartReadData[0x80]);

        // NVMe values are already normalised to GiB above, so the byte
        // totals are a straight scale (see the note in FillSmartData()).
        {
            constexpr ULONGLONG kGiB = 1024ULL * 1024ULL * 1024ULL;
            asi.HostWritesBytes =
                (asi.HostWrites > 0) ? static_cast<ULONGLONG>(asi.HostWrites) * kGiB : 0ULL;
            asi.HostReadsBytes =
                (asi.HostReads > 0) ? static_cast<ULONGLONG>(asi.HostReads) * kGiB : 0ULL;
        }

        NVMeSmartToATASmart(asi.SmartReadData, asi.Attribute, MAX_ATTRIBUTE);
        if (asi.IsNvmeThresholdSupported) {
            NVMeCompositeTemperatureSmartToATASmart(asi.SmartReadData, asi.Attribute,
                                                   MAX_ATTRIBUTE);
        }
        if (asi.IsNvmeThermalManagementSupported) {
            NVMeThermalManagementTemperatureSmartToATASmart(asi.SmartReadData, asi.Attribute,
                                                           MAX_ATTRIBUTE);
        }

        asi.AttributeCount = MAX_ATTRIBUTE;

        // upstream quirk: the vendor is only stamped when the S.M.A.R.T. read
        // succeeded — hoisting this out would label a drive whose read failed
        // as an NVMe vendor drive with IsSmartCorrect already TRUE.
        asi.SmartKeyName = L"SmartNVMe";
        asi.DiskVendorId = SSD_VENDOR_NVME;
        asi.SsdVendorString = detail::kSsdVendorString[SSD_VENDOR_NVME];
        asi.Interface = L"NVM Express";
    }

    m_vars.push_back(asi);
    return TRUE;
}

// ═══════════════════════════════════════════════════════════════════════
//  Refresh — CrystalDiskInfo AtaSmart.cpp:88-448
// ═══════════════════════════════════════════════════════════════════════

DWORD CAtaSmart::UpdateSmartInfo(DWORD index) {
    if (m_vars.empty()) return SMART_STATUS_NO_CHANGE;
    if (index >= m_vars.size()) return SMART_STATUS_NO_CHANGE;

    DRIVE_INFO& asi = m_vars[index];
    SMART_ATTRIBUTE* attribute = g_attributeShadow[index];

    // CrystalDiskInfo CAtaSmart::CheckSmartAttributeUpdate (AtaSmart.cpp:558-652).
    //
    // Defined here as a lambda rather than a free function so it can reach the
    // private static GetPowerOnHours().  It is stateful: every path that
    // reports a change copies "cur" into "pre", so it must run on every
    // successful refresh or the next diff is taken against a stale baseline.
    auto checkSmartAttributeUpdate = [&asi](SMART_ATTRIBUTE* pre, SMART_ATTRIBUTE* cur) -> DWORD {
        if (!asi.IsSmartCorrect) return SMART_STATUS_NO_CHANGE;
        if (pre == nullptr || cur == nullptr) return SMART_STATUS_NO_CHANGE;

        if (memcmp(pre, cur, sizeof(SMART_ATTRIBUTE) * MAX_ATTRIBUTE) == 0) {
            return SMART_STATUS_NO_CHANGE;
        }

        for (int i = 0; i < MAX_ATTRIBUTE; i++) {
            switch (cur[i].Id) {
            case 0x09: {  // Power on Hours
                DWORD preRawValue = detail::B8toB32le(pre[i].RawValue);
                DWORD curRawValue = detail::B8toB32le(cur[i].RawValue);

                if (asi.DiskVendorId == SSD_VENDOR_INDILINX) {
                    preRawValue = pre[i].WorstValue * 256 + pre[i].CurrentValue;
                    curRawValue = cur[i].WorstValue * 256 + cur[i].CurrentValue;
                }

                if (GetPowerOnHours(preRawValue, asi.DetectedTimeUnitType) !=
                    GetPowerOnHours(curRawValue, asi.DetectedTimeUnitType)) {
                    memcpy(pre, cur, sizeof(SMART_ATTRIBUTE) * MAX_ATTRIBUTE);
                    return SMART_STATUS_MAJOR_CHANGE;
                }
                if (GetPowerOnHours(preRawValue, asi.MeasuredTimeUnitType) !=
                    GetPowerOnHours(curRawValue, asi.MeasuredTimeUnitType)) {
                    memcpy(pre, cur, sizeof(SMART_ATTRIBUTE) * MAX_ATTRIBUTE);
                    return SMART_STATUS_MAJOR_CHANGE;
                }
                break;
            }
            case 0x0C: {  // Power On Count
                DWORD preRawValue = detail::B8toB32le(pre[i].RawValue);
                DWORD curRawValue = detail::B8toB32le(cur[i].RawValue);

                if (asi.DiskVendorId == SSD_VENDOR_INDILINX) {
                    preRawValue = pre[i].WorstValue * 256 + pre[i].CurrentValue;
                    curRawValue = cur[i].WorstValue * 256 + cur[i].CurrentValue;
                }

                if (preRawValue != curRawValue) {
                    memcpy(pre, cur, sizeof(SMART_ATTRIBUTE) * MAX_ATTRIBUTE);
                    return SMART_STATUS_MAJOR_CHANGE;
                }
                break;
            }
            case 0xBE:  // 3.9.4 or later
            case 0xC2:  // Temperature
                if (pre[i].RawValue[0] != cur[i].RawValue[0] ||
                    pre[i].CurrentValue != cur[i].CurrentValue) {
                    memcpy(pre, cur, sizeof(SMART_ATTRIBUTE) * MAX_ATTRIBUTE);
                    return SMART_STATUS_MAJOR_CHANGE;
                }
                break;
            default:
                break;
            }
        }

        // 3.4.0 — upstream reports MAJOR_CHANGE for any difference it did not
        // classify above (the MINOR_CHANGE return is commented out upstream).
        return SMART_STATUS_MAJOR_CHANGE;
    };

    for (int j = 0; j < 8; j++) {
        asi.TemperatureNVMe[j] = -1000;
    }

    // ── NVMe ─────────────────────────────────────────────────────────
    if (asi.DiskVendorId == SSD_VENDOR_NVME) {
        // Re-run the interpreter over the buffered log so the attribute list
        // is fresh even when the re-read below fails.
        NVMeSmartToATASmart(asi.SmartReadData, asi.Attribute, MAX_ATTRIBUTE);

        if (asi.IsNvmeThresholdSupported) {
            NVMeCompositeTemperatureSmartToATASmart(asi.SmartReadData, asi.Attribute,
                                                   MAX_ATTRIBUTE);
        }
        if (asi.IsNvmeThermalManagementSupported) {
            NVMeThermalManagementTemperatureSmartToATASmart(asi.SmartReadData, asi.Attribute,
                                                           MAX_ATTRIBUTE);
        }

        if (m_bNVMeStorageQuery && asi.CommandType == CMD_TYPE_NVME_STORAGE_QUERY &&
            GetSmartAttributeNVMeStorageQuery(asi.PhysicalDriveId, &asi)) {
            asi.Temperature = asi.SmartReadData[0x2] * 256 + asi.SmartReadData[0x1] - 273;
            // upstream quirk: the ceiling here is 200 C, while AddDiskNVMe()
            // uses 100 C.  The inconsistency is upstream's and is preserved.
            if (asi.Temperature == -273 || asi.Temperature > 200) {
                asi.Temperature = -1000;
            }

            for (int j = 0; j < 8; j++) {
                asi.TemperatureNVMe[j] =
                    asi.SmartReadData[200 + j * 2 + 1] * 256 +
                    asi.SmartReadData[200 + j * 2] - 273;
                if (asi.TemperatureNVMe[j] == -273 || asi.TemperatureNVMe[j] > 200) {
                    asi.TemperatureNVMe[j] = -1000;
                }
            }

            asi.Life = 100 - asi.SmartReadData[0x05];
            if (asi.Life < 0) {
                asi.Life = 0;
            }

            asi.HostReads =
                static_cast<INT>((detail::B8toB64lePtr(&asi.SmartReadData[0x20]) * 1000) >> 21);
            asi.HostWrites =
                static_cast<INT>((detail::B8toB64lePtr(&asi.SmartReadData[0x30]) * 1000) >> 21);

            // The values above are normalised to GiB; see FillSmartData() for
            // why the byte totals are a straight scale of them.
            constexpr ULONGLONG kGiB = 1024ULL * 1024ULL * 1024ULL;
            asi.HostWritesBytes =
                (asi.HostWrites > 0) ? static_cast<ULONGLONG>(asi.HostWrites) * kGiB : 0ULL;
            asi.HostReadsBytes =
                (asi.HostReads > 0) ? static_cast<ULONGLONG>(asi.HostReads) * kGiB : 0ULL;

            asi.PowerOnCount = static_cast<DWORD>(detail::B8toB64lePtr(&asi.SmartReadData[0x70]));
            asi.PowerOnHours = static_cast<INT>(detail::B8toB64lePtr(&asi.SmartReadData[0x80]));
        }

        asi.DiskStatus = CheckDiskStatus(asi);
        // upstream quirk: the NVMe branch reports a major change
        // unconditionally, whatever the re-read did.
        return SMART_STATUS_MAJOR_CHANGE;
    }

    // ── ATA / SAT / legacy ───────────────────────────────────────────
    if (asi.IsSmartEnabled && asi.IsSmartCorrect) {
        switch (asi.CommandType) {
        case CMD_TYPE_PHYSICAL_DRIVE:
            if (!GetSmartAttributePd(asi.PhysicalDriveId, asi.Target, &asi)) {
                detail::WakeUpDisk(asi.PhysicalDriveId);
                if (!GetSmartAttributePd(asi.PhysicalDriveId, asi.Target, &asi)) {
                    return SMART_STATUS_NO_CHANGE;
                }
            }
            asi.DiskStatus = CheckDiskStatus(asi);
            break;

        case CMD_TYPE_SCSI_MINIPORT:
            if (!GetSmartAttributeSi(asi.PhysicalDriveId, &asi)) {
                return SMART_STATUS_NO_CHANGE;
            }
            asi.DiskStatus = CheckDiskStatus(asi);
            break;

        case CMD_TYPE_SAT:
        case CMD_TYPE_JMICRON:
            // Unlike the physical-drive path, upstream wakes the drive *first*
            // here and does not retry.
            detail::WakeUpDisk(asi.PhysicalDriveId);
            if (!GetSmartAttributeSat(asi.PhysicalDriveId, asi.Target, &asi)) {
                return SMART_STATUS_NO_CHANGE;
            }
            asi.DiskStatus = CheckDiskStatus(asi);
            break;

        default:
            // An unknown command type never refreshes.
            return SMART_STATUS_NO_CHANGE;
        }

        return checkSmartAttributeUpdate(attribute, asi.Attribute);
    }

    return SMART_STATUS_NO_CHANGE;
}

BOOL CAtaSmart::UpdateIdInfo(DWORD index) {
    if (index >= m_vars.size()) return FALSE;

    DRIVE_INFO& asi = m_vars[index];
    BOOL flag = FALSE;

    switch (asi.CommandType) {
    case CMD_TYPE_PHYSICAL_DRIVE:
        flag = DoIdentifyDevicePd(asi.PhysicalDriveId, asi.Target, &asi.IdentifyDevice);
        break;
    case CMD_TYPE_SCSI_MINIPORT:
        flag = DoIdentifyDeviceSi(asi.PhysicalDriveId, asi.Target, &asi.IdentifyDevice);
        break;
    case CMD_TYPE_SAT:
    case CMD_TYPE_JMICRON:
        flag = DoIdentifyDeviceSat(asi.PhysicalDriveId, asi.Target, &asi.IdentifyDevice,
                                   asi.CommandType);
        break;
    default:
        return FALSE;
    }

    if (!flag) return FALSE;

    // Refresh the identity strings from the fresh buffer.  Upstream also
    // recomputes the AAM/APM support flags here; DRIVE_INFO carries no such
    // fields, so there is nothing to update.
    asi.SerialNumber = TrimCopy(asi.IdentifyDevice.A.SerialNumber,
                                sizeof(asi.IdentifyDevice.A.SerialNumber));
    asi.FirmwareRev = TrimCopy(asi.IdentifyDevice.A.FirmwareRev,
                               sizeof(asi.IdentifyDevice.A.FirmwareRev));
    asi.Model = TrimCopy(asi.IdentifyDevice.A.Model, sizeof(asi.IdentifyDevice.A.Model));

    return TRUE;
}

// ═══════════════════════════════════════════════════════════════════════
//  S.M.A.R.T. control and identify accessors
// ═══════════════════════════════════════════════════════════════════════

BOOL CAtaSmart::EnableSmart(DWORD index) {
    if (index >= m_vars.size()) return FALSE;

    const DRIVE_INFO& asi = m_vars[index];
    detail::WakeUpDisk(asi.PhysicalDriveId);

    switch (asi.CommandType) {
    case CMD_TYPE_PHYSICAL_DRIVE:
        return ControlSmartStatusPd(asi.PhysicalDriveId, asi.Target, ENABLE_SMART);
    case CMD_TYPE_SAT:
    case CMD_TYPE_JMICRON:
        return ControlSmartStatusSat(asi.PhysicalDriveId, asi.Target, ENABLE_SMART,
                                     asi.CommandType);
    default:
        return FALSE;
    }
}

BOOL CAtaSmart::DisableSmart(DWORD index) {
    if (index >= m_vars.size()) return FALSE;

    const DRIVE_INFO& asi = m_vars[index];
    detail::WakeUpDisk(asi.PhysicalDriveId);

    switch (asi.CommandType) {
    case CMD_TYPE_PHYSICAL_DRIVE:
        return ControlSmartStatusPd(asi.PhysicalDriveId, asi.Target, DISABLE_SMART);
    case CMD_TYPE_SAT:
    case CMD_TYPE_JMICRON:
        return ControlSmartStatusSat(asi.PhysicalDriveId, asi.Target, DISABLE_SMART,
                                     asi.CommandType);
    default:
        return FALSE;
    }
}

BYTE CAtaSmart::GetAamValue(DWORD index) {
    if (index >= m_vars.size()) return 0;
    return LOBYTE(m_vars[index].IdentifyDevice.A.AcoustricManagement);
}

BYTE CAtaSmart::GetApmValue(DWORD index) {
    if (index >= m_vars.size()) return 0;
    return LOBYTE(m_vars[index].IdentifyDevice.A.CurrentPowerManagement);
}

// ═══════════════════════════════════════════════════════════════════════
//  Interface classification
// ═══════════════════════════════════════════════════════════════════════
//
// CrystalDiskInfo spreads this over three places: Init() sets
// INTERFACE_TYPE_NVME / INTERFACE_TYPE_USB from the storage descriptor
// (AtaSmart.cpp:2146-2191), GetTransferMode() overrides it from identify
// words 76/88 (AtaSmart.cpp:12925), and AddDisk() calls the latter.  The
// descriptor wins for USB and NVMe, so this runs after GetTransferMode().
void CAtaSmart::SetInterfaceType(DRIVE_INFO& asi, const STORAGE_DEVICE_DESCRIPTOR* desc) {
    if (desc != nullptr) {
        switch (static_cast<DWORD>(desc->BusType)) {
        case kBusTypeUsb:
            asi.InterfaceType = INTERFACE_TYPE_USB;
            asi.Interface = L"USB";
            return;
        case kBusTypeNvme:
            asi.InterfaceType = INTERFACE_TYPE_NVME;
            asi.Interface = L"NVM Express";
            return;
        case kBusTypeSata:
            asi.InterfaceType = INTERFACE_TYPE_SATA;
            asi.Interface = L"Serial ATA";
            return;
        case kBusTypeAta:
            asi.InterfaceType = INTERFACE_TYPE_PATA;
            asi.Interface = L"Parallel ATA";
            return;
        case kBusTypeScsi:
        case kBusTypeRAID:
            asi.InterfaceType = INTERFACE_TYPE_SCSI;
            asi.Interface = L"SCSI";
            return;
        default:
            break;
        }
    }

    // No usable descriptor: keep whatever GetTransferMode() derived from the
    // identify words (INTERFACE_TYPE_PATA / _SATA / _UNKNOWN).
    if (asi.Interface.empty()) {
        switch (asi.InterfaceType) {
        case INTERFACE_TYPE_PATA: asi.Interface = L"Parallel ATA"; break;
        case INTERFACE_TYPE_SATA: asi.Interface = L"Serial ATA"; break;
        case INTERFACE_TYPE_NVME: asi.Interface = L"NVM Express"; break;
        case INTERFACE_TYPE_USB:  asi.Interface = L"USB"; break;
        default: break;
        }
    }
}


} // namespace cdi
