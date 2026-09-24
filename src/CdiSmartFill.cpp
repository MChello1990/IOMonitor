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
//  CdiSmartFill — CAtaSmart::FillSmartData() / CAtaSmart::FillSmartThreshold()
//
//  Ported verbatim from CrystalDiskInfo AtaSmart.cpp:
//
//    * CAtaSmart::FillSmartData()       AtaSmart.cpp 11734-12467
//    * CAtaSmart::FillSmartThreshold()  AtaSmart.cpp 12469-12513
//
//  FillSmartData() walks the 30 slot SMART attribute table embedded in
//  SmartReadData[], keeps the non-zero entries, and decodes the handful of
//  attributes CrystalDiskInfo gives a vendor-specific meaning to
//  (power-on hours / count, temperature, life, host reads / writes, NAND
//  writes, GBytesErased).  Everything it writes lands in DRIVE_INFO.
//
//  ── DRIVE_INFO field mapping ────────────────────────────────────────
//
//  asi->X                    -> asi->X           (parameter stays a pointer)
//
//  DetectedPowerOnHours     \  These two have no slot of their own in
//  MeasuredPowerOnHours     /  DRIVE_INFO.  The only slot is
//                              DRIVE_INFO::PowerOnHours (hours), so BOTH are
//                              written to it; the measured assignment comes
//                              last and therefore wins, which is what the UI
//                              displays.
//
//  FlagLifeNoReport          \
//  FlagLifeRawValue           |
//  FlagLifeRawValueIncrement  |
//  FlagLifeSanDiskUsbMemory   |  Not in DRIVE_INFO — they are display policy,
//  FlagLifeSanDisk0_1         |  not disk state.  They live in the
//  FlagLifeSanDisk1           |  cdi::detail::SsdLifeCtx side table keyed on
//  FlagLifeSanDiskLenovo      |  Model + SerialNumber.  Read once at the top
//  FlagLifeSanDiskCloud       |  of FillSmartData() through
//  NandWritesUnit             /  cdi::detail::LifeCtxOf(*asi).
//
//  The upstream `NandWritesUnit == NAND_WRITES_1MB` test therefore becomes
//  `ssdLife.NandWrites1MB`.
//
//  IsRawValues8 / IsRawValues7 are also absent from DRIVE_INFO.  Upstream
//  sets them in CheckSsdSupport() (AtaSmart.cpp 4212 / 4219 / 4256) purely
//  from the resolved SmartKeyName, which is the value carried in DRIVE_INFO,
//  so they can be recovered here exactly:
//      IsRawValues8 <=> SmartKeyName == "SmartIndilinx"  (SSD_VENDOR_INDILINX)
//                    || SmartKeyName == "SmartJMicron60x"
//      IsRawValues7 <=> SmartKeyName == "SmartSandForce"
//  Note SSD_VENDOR_JMICRON is shared by the JMicron 60x / 61x / 66x
//  generations; only 60x sets IsRawValues8, so the flag must NOT be derived
//  from DiskVendorId.  (The FillSmartData body in the ported line range never
//  actually consults them — they are consumed by the UI copy / update paths —
//  they are derived here for parity with CheckSsdSupport().)
//
//  DetectedTimeUnitType / MeasuredTimeUnitType / IsSsd / DiskVendorId /
//  Model / FirmwareRev / SmartReadData / Attribute[] / AttributeCount /
//  IsNVMe / HostReadsWritesUnit / TemperatureMultiplier / IsWord88 /
//  IsNvmeThresholdSupported all exist in DRIVE_INFO unchanged.
//
//  ── HOST_READS_WRITES_* ladders ─────────────────────────────────────
//
//  Upstream repeats the 512B / 1MB / 16MB / 32MB / GB arithmetic six times
//  (0xE9, 0xF1 x3, 0xF2 x3) with different framing, different vendor sets and
//  deliberately different branch coverage: 0xF1 offers a 1MB step and an
//  explicit (empty) fall-through, 0xF2 has no 1MB step and folds GB into its
//  final `else`.  All six are reproduced as written — the divisors are never
//  normalised and the asymmetries are marked.  The HOST_READS_WRITES_GB
//  branches read the raw field with the SIGNED B8toINTle because a 32-bit
//  GiB counter can have bit 31 set; an unsigned reader would flip the sign of
//  HostWrites / HostReads.
//
//  HostWrites / HostReads keep upstream's -1 sentinel.  The sentinel is not
//  set here: upstream initialises it in the caller (AtaSmart.cpp 2754-2755 in
//  AddDisk / 3902-3903 in GetSmartAttribute, outside this port's scope), and
//  FillSmartData only ever overwrites it from the ladders above.  The empty
//  `else` branches therefore leave the sentinel intact, and the byte totals
//  at the end of the function test `> 0` so a sentinel becomes 0 bytes.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmartDetail.h"
#include "SmartDebug.h"

#include <cstring>
#include <cwchar>

namespace cdi {

namespace {

// CString::CompareNoCase() == 0 — case-insensitive equality against a literal.
// Used by the Intel firmware special case in the 0x09 branch.
bool EqualsNoCase(const std::wstring& s, const wchar_t* literal) {
    const size_t n = ::wcslen(literal);
    if (s.size() != n) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        if (::towupper(s[i]) != ::towupper(literal[i])) {
            return false;
        }
    }
    return true;
}

} // namespace

// ── CAtaSmart::FillSmartData — AtaSmart.cpp 11734-12467 ───────────────
BOOL CAtaSmart::FillSmartData(DRIVE_INFO* asi)
{
    // FlagLife* / NandWritesUnit side channel — read once, see banner.
    const ::cdi::detail::SsdLifeCtx ssdLife = ::cdi::detail::LifeCtxOf(*asi);

    // Derived from SmartKeyName, not DiskVendorId — see banner.
    const BOOL rawValues8 = (asi->SmartKeyName == L"SmartIndilinx" ||
                             asi->SmartKeyName == L"SmartJMicron60x");
    const BOOL rawValues7 = (asi->SmartKeyName == L"SmartSandForce");
    // The ported body below does not consult either flag; they are derived
    // here only so the mapping with upstream CheckSsdSupport() is explicit.
    (void)rawValues8;
    (void)rawValues7;

    SMART_TRACE_EVENT("FillSmartData", SmartTraceCategory::ATTRIBUTE_PARSE,
                      "FillSmartData entry");

    asi->AttributeCount = 0;
    int j = 0;
    for (int i = 0; i < MAX_ATTRIBUTE; i++)
    {
        DWORD rawValue = 0;
        memcpy(    &(asi->Attribute[j]),
            &(asi->SmartReadData[i * sizeof(SMART_ATTRIBUTE) + 2]), sizeof(SMART_ATTRIBUTE));

        if (asi->Attribute[j].Id != 0)
        {
            switch (asi->Attribute[j].Id)
            {
            case 0x09: // Power on Hours
                rawValue = cdi::detail::B8toB32le(asi->Attribute[j].RawValue);
                /*    MAKELONG(
                    MAKEWORD(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1]),
                    MAKEWORD(asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3])
                    );*/
                if (asi->DiskVendorId == SSD_VENDOR_INDILINX)
                {
                    rawValue = asi->Attribute[j].WorstValue * 256 + asi->Attribute[j].CurrentValue;
                }
                // Intel SSD 520 Series and etc...
                else if (cdi::detail::StartsWith(asi->Model, L"Intel"))
                {
                    // upstream quirk: firmware-gated prior to the generic Intel
                    // time-unit heuristic.  Three firmware revisions report
                    // attribute 09 in MINUTES and are converted to hours; every
                    // other Intel firmware falls through to the 0x0DA000
                    // millisecond heuristic below.
                    const std::wstring fw = asi->FirmwareRev;
                    if (EqualsNoCase(fw, L"522ABBF0") ||
                        EqualsNoCase(fw, L"520i") ||
                        EqualsNoCase(fw, L"LB3i"))
                    {
                        // Intel exceptions: attribute 09 is minutes
                        ULONGLONG minutes =
                            ((ULONGLONG)asi->Attribute[j].RawValue[5] << 40) |
                            ((ULONGLONG)asi->Attribute[j].RawValue[4] << 32) |
                            ((ULONGLONG)asi->Attribute[j].RawValue[3] << 24) |
                            ((ULONGLONG)asi->Attribute[j].RawValue[2] << 16) |
                            ((ULONGLONG)asi->Attribute[j].RawValue[1] << 8) |
                            ((ULONGLONG)asi->Attribute[j].RawValue[0]);

                        rawValue = (DWORD)(minutes / 60); // hours
                    }
                    else if (
                        (asi->DetectedTimeUnitType == POWER_ON_MILLI_SECONDS)
                        || (asi->DetectedTimeUnitType == POWER_ON_HOURS && rawValue >= 0x0DA000)
                        || (rawValue >= 0x0DA000)
                        )
                    {
                        asi->MeasuredTimeUnitType = POWER_ON_MILLI_SECONDS;
                        int value = 0;
                        rawValue = value = asi->Attribute[j].RawValue[2] * 256 * 256
                            + asi->Attribute[j].RawValue[1] * 256
                            + asi->Attribute[j].RawValue[0] - 0x0DA753;
                        if (value < 0)
                        {
                            rawValue = 0;
                        }
                    }
                }

                asi->PowerOnRawValue = static_cast<INT>(rawValue);
                // DRIVE_INFO has a single PowerOnHours slot: upstream's
                // DetectedPowerOnHours and MeasuredPowerOnHours both land in
                // it, measured last so the measured value wins.
                asi->PowerOnHours = static_cast<INT>(GetPowerOnHours(rawValue, asi->DetectedTimeUnitType));
                asi->PowerOnHours = static_cast<INT>(GetPowerOnHours(rawValue, asi->MeasuredTimeUnitType));
                break;
            case 0x0C: // Power On Count
                rawValue = cdi::detail::B8toB32le(asi->Attribute[j].RawValue);
                /*MAKELONG(
                    MAKEWORD(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1]),
                    MAKEWORD(asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3])
                    );*/
                if (asi->DiskVendorId == SSD_VENDOR_INDILINX)
                {
                    rawValue = asi->Attribute[j].WorstValue * 256 + asi->Attribute[j].CurrentValue;
                }
                asi->PowerOnCount = rawValue;
                break;
            case 0xBE:
                if (asi->Attribute[j].RawValue[0] > 0 && asi->Attribute[j].RawValue[0] < 100)
                {
                    asi->Temperature = asi->Attribute[j].RawValue[0];
                }
                break;
            case 0xBF: // Clean PowerOff Count for Sandisk/WD CloudSpeed SSD
                if (asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                {
                    // Use Clean Shutdowns to calculate Power On Count
                    rawValue = cdi::detail::B8toB32le(asi->Attribute[j].RawValue);
                    asi->PowerOnCount = rawValue + 1;
                }
                break;
            case 0xC0: // UnClean PowerOff Count for Sandisk/WD CloudSpeed SSD
                if (asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                {
                    // Use UnClean Shutdowns to calculate Power On Count
                    rawValue = cdi::detail::B8toB32le(asi->Attribute[j].RawValue);
                    asi->PowerOnCount += rawValue;
                }
                break;
            case 0xC2: // Temperature
                if (cdi::detail::StartsWith(asi->Model, L"SAMSUNG SV") && (asi->Attribute[j].RawValue[1] != 0 || asi->Attribute[j].RawValue[0] > 70))
                {
                    asi->Temperature = cdi::detail::B8toB16le(asi->Attribute[j].RawValue) / 10; //MAKEWORD(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1]) / 10;
                }
                else if (asi->Attribute[j].RawValue[0] > 0 && asi->TemperatureMultiplier < 1.0) //(asi->DiskVendorId == SSD_VENDOR_SANDFORCE)
                {
                    asi->Temperature = static_cast<INT>(static_cast<DWORD>(asi->Attribute[j].RawValue[0] * asi->TemperatureMultiplier));
                }
                else if (asi->Attribute[j].RawValue[0] > 0)
                {
                    asi->Temperature = asi->Attribute[j].RawValue[0];
                }

                if (asi->Temperature >= 100)
                {
                    asi->Temperature = -1000;
                }
                break;
            case 0xF3: // Temperature for YMTC
                if (asi->DiskVendorId == SSD_VENDOR_YMTC)
                {
                    if (asi->Attribute[j].RawValue[0] > 0)
                    {
                        asi->Temperature = asi->Attribute[j].RawValue[0];
                    }

                    if (asi->Temperature >= 100)
                    {
                        asi->Temperature = -1000;
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_INTEL)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                            asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32); //  65536 * 512 / 1024 / 1024 / 1024;
                }
                break;
            case 0xBB:
                if (asi->DiskVendorId == SSD_VENDOR_MTRON)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xCA:
                if (asi->DiskVendorId == SSD_VENDOR_MICRON || asi->DiskVendorId == SSD_VENDOR_MICRON_MU03 || asi->DiskVendorId == SSD_VENDOR_INTEL_DC || asi->DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xD1:
                if (asi->DiskVendorId == SSD_VENDOR_INDILINX)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xC9:
                if (asi->DiskVendorId == SSD_VENDOR_SANDISK_HP || asi->DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS)
                {
                    int life = -1;
                    life = asi->Attribute[j].CurrentValue;
                    if (life <= 0 || life > 100) { life = -1; }
                    asi->Life = life;
                }
                break;
            case 0xE6:
                if (asi->DiskVendorId == SSD_VENDOR_WDC || asi->DiskVendorId == SSD_VENDOR_SANDISK)
                {
                    int life = -1;
                    if (ssdLife.FlagLifeSanDiskUsbMemory)
                    {
                        life = -1;
                    }
                    else if (ssdLife.FlagLifeSanDisk0_1)
                    {
                        life = 100 - (asi->Attribute[j].RawValue[1] * 256 + asi->Attribute[j].RawValue[0]) / 100;
                    }
                    else if (ssdLife.FlagLifeSanDisk1)
                    {
                        life = 100 - asi->Attribute[j].RawValue[1];
                    }
                    else if (ssdLife.FlagLifeSanDiskLenovo)
                    {
                        life = asi->Attribute[j].CurrentValue;
                    }
                    else
                    {
                        life = 100 - asi->Attribute[j].RawValue[1];
                    }

                    if (life <= 0 || life > 100) { life = -1; }

                    asi->Life = life;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi->DiskVendorId == SSD_VENDOR_SANDISK_DELL)
                {
                    int life = -1;
                    // upstream quirk: this range check is executed before `life`
                    // is loaded from the attribute, so it can never fire — the
                    // current value is stored unchecked.
                    if (life <= 0 || life > 100) { life = -1; }
                    life = asi->Attribute[j].CurrentValue;
                    asi->Life = life;
                }
                break;
            case 0xE8:
                if (asi->DiskVendorId == SSD_VENDOR_PLEXTOR)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_OCZ)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                break;
            case 0xE9:
                if (asi->DiskVendorId == SSD_VENDOR_INTEL || asi->DiskVendorId == SSD_VENDOR_OCZ || asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi->DiskVendorId == SSD_VENDOR_SKHYNIX)
                {
                    if (ssdLife.FlagLifeRawValue)
                    {
                        asi->Life = asi->Attribute[j].RawValue[0];
                    }
                    else
                    {
                        asi->Life = asi->Attribute[j].CurrentValue;
                    }
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                // NEW: Samsung enterprise SATA (Media_Wearout_Indicator normalized value = % life remaining)
                else if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG && cdi::detail::IsSamsungEnterpriseModel(asi->Model))
                {
                                // Samsung enterprise SATA: ID 0xE9 (233) normalized VALUE = % life remaining.
                                asi->Life = asi->Attribute[j].CurrentValue;
                                if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                else if ((asi->DiskVendorId == SSD_VENDOR_SANDISK ||
                        asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO ||
                        asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                    && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    if (ssdLife.NandWrites1MB) // upstream: asi->NandWritesUnit == NAND_WRITES_1MB
                    {
                        asi->NandWrites = static_cast<INT>(cdi::detail::B8toB32le(asi->Attribute[j].RawValue) / 1024);
                    }
                    else
                    {
                        asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); // (INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_PLEXTOR || asi->DiskVendorId == SSD_VENDOR_KINGSTON || asi->DiskVendorId == SSD_VENDOR_WDC || asi->DiskVendorId == SSD_VENDOR_SSSTC || asi->DiskVendorId == SSD_VENDOR_SEAGATE || asi->DiskVendorId == SSD_VENDOR_YMTC || asi->DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC)
                {
                    asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_JMICRON || asi->DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL)
                {
                    asi->NandWrites = static_cast<INT>(
                        cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                            asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_MAXIOTEK)
                {
                    if (asi->HostReadsWritesUnit == HOST_READS_WRITES_512B)
                    {
                        asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 2 / 1024 / 1024);
                    }
                    else
                    {
                        asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                }
                break;
            case 0xE1:
                if (asi->DiskVendorId == SSD_VENDOR_INTEL)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32); //  65536 * 512 / 1024 / 1024 / 1024;
                }
                break;
            case 0xEA:
                if (asi->DiskVendorId == SSD_VENDOR_KINGSTON || asi->DiskVendorId == SSD_VENDOR_SEAGATE
                    ||  (asi->DiskVendorId == SSD_VENDOR_SKHYNIX && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                    )
                {
                    asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                break;
            case 0xEB:
                if (asi->DiskVendorId == SSD_VENDOR_INTEL_DC)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                break;
            case 0xF1:
                if (asi->DiskVendorId == SSD_GENERAL)
                {
                    if (asi->HostReadsWritesUnit == HOST_READS_WRITES_512B)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 2 / 1024 / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_1MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 64);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                    {
                        asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                    else
                    {
                        // upstream quirk: HOST_READS_WRITES_UNKNOWN leaves
                        // HostWrites untouched (the -1 sentinel survives).
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_TOSHIBA && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_INTEL_DC)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        //(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                //asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_INTEL || asi->DiskVendorId == SSD_VENDOR_TOSHIBA || asi->DiskVendorId == SSD_VENDOR_KIOXIA || asi->DiskVendorId == SSD_VENDOR_SILICONMOTION)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                // upstream quirk: this ladder (unlike the 0xF2 one) carries a
                // 1MB step, lists PHISON, and ends in a bare `else` that leaves
                // HostWrites at the -1 sentinel.
                else if (asi->DiskVendorId == SSD_VENDOR_SANDFORCE || asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi->DiskVendorId == SSD_VENDOR_CORSAIR || asi->DiskVendorId == SSD_VENDOR_KINGSTON || asi->DiskVendorId == SSD_VENDOR_REALTEK
                    ||   asi->DiskVendorId == SSD_VENDOR_WDC || asi->DiskVendorId == SSD_VENDOR_SSSTC || asi->DiskVendorId == SSD_VENDOR_SKHYNIX || asi->DiskVendorId == SSD_VENDOR_PHISON || asi->DiskVendorId == SSD_VENDOR_SEAGATE || asi->DiskVendorId == SSD_VENDOR_MARVELL
                    ||   asi->DiskVendorId == SSD_VENDOR_MAXIOTEK || asi->DiskVendorId == SSD_VENDOR_YMTC || asi->DiskVendorId == SSD_VENDOR_SCY || asi->DiskVendorId == SSD_VENDOR_RECADATA || asi->DiskVendorId == SSD_VENDOR_MICRON_MU03
                    ||   asi->DiskVendorId == SSD_VENDOR_SANDISK_HP || asi->DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS || asi->DiskVendorId == SSD_VENDOR_SANDISK_DELL || asi->DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL
                    )
                {
                    if (asi->HostReadsWritesUnit == HOST_READS_WRITES_512B)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5]) */
                            / 2 / 1024 / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_1MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 64);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                    {
                        asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                    }
                    else
                    {
                        asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG || asi->DiskVendorId == SSD_VENDOR_APACER || asi->DiskVendorId == SSD_VENDOR_JMICRON)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_PLEXTOR)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                         / 32);
                }
                // upstream quirk: SANDISK does NOT appear in the big ladder
                // above; it is handled here, GB-first, before the generic
                // SANDISK fall-through.
                else if ((asi->DiskVendorId == SSD_VENDOR_SANDISK ||
                        asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                    && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SANDISK)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                break;
            case 0xF2:
                if (asi->DiskVendorId == SSD_GENERAL)
                {
                    if (asi->HostReadsWritesUnit == HOST_READS_WRITES_512B)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 2 / 1024 / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 64);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                    {
                        asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                    else
                    {
                        // upstream quirk: the 0xF2 SSD_GENERAL ladder has no
                        // HOST_READS_WRITES_1MB step, so a 1MB drive is left at
                        // the -1 sentinel here (0xF1 does have the step).
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_TOSHIBA && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue);
                }
                // upstream quirk: no KIOXIA here (the 0xF1 twin has it).
                else if (asi->DiskVendorId == SSD_VENDOR_INTEL || asi->DiskVendorId == SSD_VENDOR_TOSHIBA || asi->DiskVendorId == SSD_VENDOR_SILICONMOTION)
                {
                    asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                // upstream quirk: no PHISON and no 1MB step here; GB and
                // UNKNOWN both land in the final `else` (signed read).
                else if (asi->DiskVendorId == SSD_VENDOR_SANDFORCE || asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi->DiskVendorId == SSD_VENDOR_CORSAIR || asi->DiskVendorId == SSD_VENDOR_KINGSTON || asi->DiskVendorId == SSD_VENDOR_REALTEK
                    ||   asi->DiskVendorId == SSD_VENDOR_WDC || asi->DiskVendorId == SSD_VENDOR_SSSTC || asi->DiskVendorId == SSD_VENDOR_SKHYNIX || asi->DiskVendorId == SSD_VENDOR_SEAGATE || asi->DiskVendorId == SSD_VENDOR_MARVELL
                    ||   asi->DiskVendorId == SSD_VENDOR_MAXIOTEK || asi->DiskVendorId == SSD_VENDOR_YMTC || asi->DiskVendorId == SSD_VENDOR_SCY || asi->DiskVendorId == SSD_VENDOR_RECADATA || asi->DiskVendorId == SSD_VENDOR_MICRON_MU03
                    ||   asi->DiskVendorId == SSD_VENDOR_SANDISK_HP || asi->DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS || asi->DiskVendorId == SSD_VENDOR_SANDISK_DELL || asi->DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL
                    )
                {
                    if (asi->HostReadsWritesUnit == HOST_READS_WRITES_512B)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 2 / 1024 / 1024);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 64);
                    }
                    else if (asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                    {
                        asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                            /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                    asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                            / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                    }
                    else
                    {
                        asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                    }
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG || asi->DiskVendorId == SSD_VENDOR_JMICRON)
                {
                    asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_PLEXTOR)
                {
                    asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 32);
                }
                else if ((asi->DiskVendorId == SSD_VENDOR_SANDISK ||
                    asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                    && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SANDISK)
                {
                    asi->HostReads = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                            asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5]) */
                        / 2 / 1024 / 1024);
                }
                break;
            case 0xF9:
                if (asi->DiskVendorId == SSD_VENDOR_INTEL || asi->DiskVendorId == SSD_VENDOR_REALTEK || asi->DiskVendorId == SSD_VENDOR_WDC || (asi->DiskVendorId == SSD_VENDOR_SANDISK && asi->HostReadsWritesUnit == HOST_READS_WRITES_GB)
                || asi->DiskVendorId == SSD_VENDOR_SANDISK_HP || asi->DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi->DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS)
                {
                    asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 64 / 1024); // 16 / 1024 / 1024
                }
                break;
            case 0xFA:
                if (asi->DiskVendorId == SSD_VENDOR_REALTEK)
                {
                    asi->NandWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                break;
            case 0x64:
                if (asi->DiskVendorId == SSD_VENDOR_SANDFORCE)
                {
                    asi->GBytesErased = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                break;
            case 0xAD:
                if (asi->DiskVendorId == SSD_VENDOR_TOSHIBA || asi->DiskVendorId == SSD_VENDOR_KIOXIA)
                {
                    asi->Life = asi->Attribute[j].CurrentValue - 100;
                    if (asi->Life <= 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xB1:
                if (asi->DiskVendorId == SSD_VENDOR_SAMSUNG)
                {
                    asi->WearLevelingCount = cdi::detail::B8toINTle(asi->Attribute[j].RawValue);
                        /*(INT)MAKELONG(
                        MAKEWORD(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1]),
                        MAKEWORD(asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3])
                        );*/
                    asi->Life = asi->Attribute[j].CurrentValue;
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xE7:
                if (asi->DiskVendorId == SSD_VENDOR_SANDFORCE || asi->DiskVendorId == SSD_VENDOR_CORSAIR || asi->DiskVendorId == SSD_VENDOR_KINGSTON || asi->DiskVendorId == SSD_VENDOR_SKHYNIX || asi->DiskVendorId == SSD_VENDOR_REALTEK
                ||  asi->DiskVendorId == SSD_VENDOR_SANDISK || asi->DiskVendorId == SSD_VENDOR_SSSTC || asi->DiskVendorId == SSD_VENDOR_APACER || asi->DiskVendorId == SSD_VENDOR_JMICRON || asi->DiskVendorId == SSD_VENDOR_PHISON || asi->DiskVendorId == SSD_VENDOR_SEAGATE
                ||  asi->DiskVendorId == SSD_VENDOR_MAXIOTEK || asi->DiskVendorId == SSD_VENDOR_YMTC || asi->DiskVendorId == SSD_VENDOR_SCY || asi->DiskVendorId == SSD_VENDOR_RECADATA || asi->DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL)
                {
                    if (ssdLife.FlagLifeNoReport)
                    {
                        asi->Life = -1;
                    }
                    else if (ssdLife.FlagLifeRawValueIncrement)
                    {
                        asi->Life = 100 - asi->Attribute[j].RawValue[0];
                    }
                    else if (ssdLife.FlagLifeRawValue)
                    {
                        asi->Life = asi->Attribute[j].RawValue[0];
                    }
                    else
                    {
                        asi->Life = asi->Attribute[j].CurrentValue;
                    }
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xA9:
                if (asi->DiskVendorId == SSD_VENDOR_REALTEK || (asi->DiskVendorId == SSD_VENDOR_KINGSTON && asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB) || asi->DiskVendorId == SSD_VENDOR_SILICONMOTION)
                {
                    if (ssdLife.FlagLifeRawValueIncrement)
                    {
                        asi->Life = 100 - asi->Attribute[j].RawValue[0];
                    }
                    else if (ssdLife.FlagLifeRawValue)
                    {
                        asi->Life = asi->Attribute[j].RawValue[0];
                    }
                    else
                    {
                        asi->Life = asi->Attribute[j].CurrentValue;
                    }
                    if (asi->Life < 0 || asi->Life > 100) { asi->Life = -1; }
                }
                break;
            case 0xC6:
                if (asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
                {
                    asi->HostReads = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                break;
            case 0xC7:
                if (asi->DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
                {
                    asi->HostWrites = cdi::detail::B8toINTle(asi->Attribute[j].RawValue); //(INT)B8toB32(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2], asi->Attribute[j].RawValue[3]);
                }
                break;
            case 0xF5:
                // Percent Drive Life Remaining (SanDisk/WD CloudSpeed)
                if (asi->DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
                {
                    asi->Life = asi->Attribute[j].CurrentValue;
                }

                // NAND Page Size = 8KBytes
                // http://www.overclock.net/t/1145150/official-crucial-ssd-owners-club/1290
                else if (asi->DiskVendorId == SSD_VENDOR_MICRON)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        * 8 / 1024 / 1024);
                }
                else if (asi->DiskVendorId == SSD_VENDOR_MICRON_MU03)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        ) / 32;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_KINGSTON && asi->HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        ) / 32;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SILICONMOTION)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        ) / 32;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_SCY)
                {
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        ) / 32;
                }
                else if (asi->DiskVendorId == SSD_VENDOR_RECADATA)
                {
                    // upstream quirk: RECADATA takes the raw 64-bit count with
                    // no scaling at all.
                    asi->NandWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        );
                }
                break;
            case 0xF6:
                if (asi->DiskVendorId == SSD_VENDOR_MICRON || asi->DiskVendorId == SSD_VENDOR_MICRON_MU03)
                {
                    asi->HostWrites = static_cast<INT>(cdi::detail::B8toB64le(asi->Attribute[j].RawValue)
                        /*B8toB64(asi->Attribute[j].RawValue[0], asi->Attribute[j].RawValue[1], asi->Attribute[j].RawValue[2],
                                asi->Attribute[j].RawValue[3], asi->Attribute[j].RawValue[4], asi->Attribute[j].RawValue[5])*/
                        / 2 / 1024 / 1024);
                }
                break;
            default:
                break;
            }
            j++;
        }
    }
    asi->AttributeCount = j;

    // HostWrites/HostReads are ALWAYS normalised to GiB by the ladders above,
    // whatever HostReadsWritesUnit says; that unit only describes how the RAW
    // attribute field is encoded.  IOMonitor's throughput graph wants bytes, so
    // convert once here rather than per consumer.
    constexpr ULONGLONG kGiB = 1024ULL * 1024ULL * 1024ULL;
    asi->HostWritesBytes = (asi->HostWrites > 0) ? static_cast<ULONGLONG>(asi->HostWrites) * kGiB : 0ULL;
    asi->HostReadsBytes  = (asi->HostReads  > 0) ? static_cast<ULONGLONG>(asi->HostReads)  * kGiB : 0ULL;

    if (asi->AttributeCount > 0)
    {
        SMART_TRACE_EVENT("FillSmartData", SmartTraceCategory::ATTRIBUTE_PARSE,
                          "FillSmartData: attributes parsed");
    }
    else
    {
        SMART_TRACE_EVENT("FillSmartData", SmartTraceCategory::ATTRIBUTE_PARSE,
                          "FillSmartData: no attributes");
    }

    if (asi->AttributeCount > 0)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

// ── CAtaSmart::FillSmartThreshold — AtaSmart.cpp 12469-12513 ──────────
BOOL CAtaSmart::FillSmartThreshold(DRIVE_INFO* asi)
{
    SMART_TRACE_EVENT("FillSmartThreshold", SmartTraceCategory::ATTRIBUTE_PARSE,
                      "FillSmartThreshold entry");

    // 2016/04/18
    // https://github.com/hiyohiyo/CrystalDiskInfo/issues/1
    int count = 0;
    for (int i = 0; i < MAX_ATTRIBUTE; i++)
    {
        SMART_THRESHOLD* pst = (SMART_THRESHOLD*)&(asi->SmartReadThreshold[i * sizeof(SMART_THRESHOLD) + 2]);
        if (pst->Id != 0)
        {
            for (DWORD j = 0; j < asi->AttributeCount; j++)
            {
                if (pst->Id == asi->Attribute[j].Id)
                {
                    memcpy(&(asi->Threshold[j]), pst, sizeof(SMART_THRESHOLD));
                    count++;
                }
            }
        }
    }

    // 2013/04/13 Added P400e SSD SMART Implementation support
    // Threshold = Attribute[].Reserved
    if (asi->DiskVendorId == SSD_VENDOR_MICRON && count == 0)
    {
        for (int i = 0; i < MAX_ATTRIBUTE; i++)
        {
            if (asi->Attribute[i].Reserved > 0)
            {
                asi->Threshold[i].Id = asi->Attribute[i].Id;
                asi->Threshold[i].ThresholdValue = asi->Attribute[i].Reserved;
                count++;
            }
        }
        SMART_TRACE_EVENT("FillSmartThreshold", SmartTraceCategory::ATTRIBUTE_PARSE,
                          "FillSmartThreshold: Micron P400e thresholds rebuilt from Attribute[].Reserved");
    }

    // 2023/02/19 Disabled Threshold Check
    // if(count > 0)
    if (asi->AttributeCount > 0)
    {
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

} // namespace cdi
