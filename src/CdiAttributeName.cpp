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

#include "CdiAttributeName.h"
#include "CdiSmartDetail.h"

#include <cstdio>
#include <cstring>
#include <cwchar>

namespace cdi {

namespace {

// ── CrystalDiskInfo attributeString[] — key per DiskVendorId ──────────
const char* const kSmartKeyByVendor[] = {
    "Smart",                    //  0 HDD_GENERAL
    "SmartSsd",                 //  1 SSD_GENERAL
    "SmartMtron",               //  2 SSD_VENDOR_MTRON
    "SmartIndilinx",            //  3 SSD_VENDOR_INDILINX
    "SmartJMicron",             //  4 SSD_VENDOR_JMICRON
    "SmartIntel",               //  5 SSD_VENDOR_INTEL
    "SmartSamsung",             //  6 SSD_VENDOR_SAMSUNG
    "SmartSandForce",           //  7 SSD_VENDOR_SANDFORCE
    "SmartMicron",              //  8 SSD_VENDOR_MICRON
    "SmartOcz",                 //  9 SSD_VENDOR_OCZ
    "SmartSeagate",             // 10 SSD_VENDOR_SEAGATE
    "SmartWdc",                 // 11 SSD_VENDOR_WDC
    "SmartPlextor",             // 12 SSD_VENDOR_PLEXTOR
    "SmartSanDisk",             // 13 SSD_VENDOR_SANDISK
    "SmartOczVector",           // 14 SSD_VENDOR_OCZ_VECTOR
    "SmartToshiba",             // 15 SSD_VENDOR_TOSHIBA
    "SmartCorsair",             // 16 SSD_VENDOR_CORSAIR
    "SmartKingston",            // 17 SSD_VENDOR_KINGSTON
    "SmartMicronMU03",          // 18 SSD_VENDOR_MICRON_MU03
    "SmartNVMe",                // 19 SSD_VENDOR_NVME
    "SmartRealtek",             // 20 SSD_VENDOR_REALTEK
    "SmartSKhynix",             // 21 SSD_VENDOR_SKHYNIX
    "SmartKioxia",              // 22 SSD_VENDOR_KIOXIA
    "SmartSsstc",               // 23 SSD_VENDOR_SSSTC
    "SmartIntelDc",             // 24 SSD_VENDOR_INTEL_DC
    "SmartApacer",              // 25 SSD_VENDOR_APACER
    "SmartSiliconMotion",       // 26 SSD_VENDOR_SILICONMOTION
    "SmartPhison",              // 27 SSD_VENDOR_PHISON
    "SmartMarvell",             // 28 SSD_VENDOR_MARVELL
    "SmartMaxiotek",            // 29 SSD_VENDOR_MAXIOTEK
    "SmartYmtc",                // 30 SSD_VENDOR_YMTC
    "SmartScy",                 // 31 SSD_VENDOR_SCY
    nullptr                     // 32, 33, 34 (JMicron 60x/61x/66x handled below)
};

std::wstring Widen(const char* s) {
    if (!s || !*s) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (len <= 1) return std::wstring();
    std::wstring out(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, out.data(), len);
    return out;
}

const char* VendorKey(DWORD vendorId) {
    if (vendorId == SSD_VENDOR_NVME) return "SmartNVMe";

    switch (vendorId) {
    case SSD_VENDOR_JMICRON_60X:                 return "SmartJMicron60x";
    case SSD_VENDOR_JMICRON_61X:                 return "SmartJMicron61x";
    case SSD_VENDOR_JMICRON_66X:                 return "SmartJMicron66x";
    case SSD_VENDOR_SEAGATE_IRON_WOLF:           return "SmartSeagateIronWolf";
    case SSD_VENDOR_SEAGATE_BARRA_CUDA:          return "SmartSeagateBarraCuda";
    case SSD_VENDOR_SANDISK_GB:                  return "SmartSanDiskGb";
    case SSD_VENDOR_KINGSTON_SUV:                return "SmartKingstonSuv";
    case SSD_VENDOR_KINGSTON_KC600:              return "SmartKingstonKC600";
    case SSD_VENDOR_KINGSTON_DC500:              return "SmartKingstonDC500";
    case SSD_VENDOR_KINGSTON_SA400:              return "SmartKingstonSA400";
    case SSD_VENDOR_RECADATA:                    return "SmartRecadata";
    case SSD_VENDOR_SANDISK_DELL:                return "SmartSanDiskDell";
    case SSD_VENDOR_SANDISK_HP:                  return "SmartSanDiskHp";
    case SSD_VENDOR_SANDISK_HP_VENUS:            return "SmartSanDiskHpVenus";
    case SSD_VENDOR_SANDISK_LENOVO:              return "SmartSanDiskLenovo";
    case SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS:  return "SmartSanDiskLenovoHelenVenus";
    case SSD_VENDOR_SANDISK_CLOUD:               return "SmartSanDiskCloud";
    case SSD_VENDOR_SILICONMOTION_CVC:           return "SmartSiliconMotionCVC";
    case SSD_VENDOR_ADATA_INDUSTRIAL:            return "SmartAdataIndustrial";
    default: break;
    }

    if (vendorId < (sizeof(kSmartKeyByVendor) / sizeof(kSmartKeyByVendor[0]))) {
        return kSmartKeyByVendor[vendorId];
    }
    return nullptr;
}

// ── Temperature extracted exactly like CAtaSmart::FillSmartData() ─────
int ExtractTemperature(const DRIVE_INFO& asi, BYTE id, const SMART_ATTRIBUTE& attr) {
    if (attr.RawValue[0] == 0) return -1000;

    switch (id) {
    case 0xBE:
        if (attr.RawValue[0] > 0 && attr.RawValue[0] < 100)
            return attr.RawValue[0];
        return -1000;
    case 0xC2:
        if (asi.Model.compare(0, 9, L"SAMSUNG SV") == 0 &&
            (attr.RawValue[1] != 0 || attr.RawValue[0] > 70)) {
            return B8toB16le(attr.RawValue) / 10;
        }
        if (asi.TemperatureMultiplier < 1.0)
            return static_cast<int>(attr.RawValue[0] * asi.TemperatureMultiplier);
        return attr.RawValue[0];
    case 0xE7:
    case 0xF3:
        return attr.RawValue[0];
    default:
        break;
    }
    return -1000;
}

// Remaining-life attribute ids per vendor, mirroring CheckDiskStatus().
bool IsLifeAttribute(const DRIVE_INFO& asi, BYTE id) {
    switch (id) {
    case 0xA9:
        return asi.DiskVendorId == SSD_VENDOR_REALTEK ||
               asi.DiskVendorId == SSD_VENDOR_SILICONMOTION ||
               (asi.DiskVendorId == SSD_VENDOR_KINGSTON &&
                asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB);
    case 0xAD: return asi.DiskVendorId == SSD_VENDOR_KIOXIA;
    case 0xB1: return asi.DiskVendorId == SSD_VENDOR_SAMSUNG;
    case 0xBB: return asi.DiskVendorId == SSD_VENDOR_MTRON || asi.IsSsd;
    case 0xCA: return asi.DiskVendorId == SSD_VENDOR_MICRON ||
                      asi.DiskVendorId == SSD_VENDOR_MICRON_MU03 ||
                      asi.DiskVendorId == SSD_VENDOR_INTEL_DC ||
                      asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC;
    case 0xD1: return asi.DiskVendorId == SSD_VENDOR_INDILINX;
    case 0xE6: return asi.DiskVendorId == SSD_VENDOR_SANDFORCE ||
                      asi.DiskVendorId == SSD_VENDOR_WDC ||
                      asi.DiskVendorId == SSD_VENDOR_SANDISK ||
                      asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO ||
                      asi.DiskVendorId == SSD_VENDOR_SANDISK_DELL;
    case 0xE7: return asi.IsSsd;
    case 0xE8: return true;   // E8 = Remaining Life / Available Reserved Space
    case 0xE9: return asi.DiskVendorId == SSD_VENDOR_INTEL ||
                      asi.DiskVendorId == SSD_VENDOR_OCZ ||
                      asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR ||
                      asi.DiskVendorId == SSD_VENDOR_SKHYNIX ||
                      asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS ||
                      asi.DiskVendorId == SSD_VENDOR_SAMSUNG;
    case 0xFF: return true;   // CrystalDiskInfo assigned "Remaining Life"
    default:   return false;
    }
}

ULONGLONG RawLe48(const SMART_ATTRIBUTE& attr) {
    ULONGLONG v = 0;
    for (int i = 5; i >= 0; --i) v = (v << 8) | attr.RawValue[i];
    return v;
}

std::wstring FormatBytes(ULONGLONG bytes) {
    wchar_t buf[64];
    const wchar_t* unit = L"B";
    double v = static_cast<double>(bytes);
    if (v >= 1024.0 * 1024 * 1024 * 1024) { v /= 1024.0 * 1024 * 1024 * 1024; unit = L"TB"; }
    else if (v >= 1024.0 * 1024 * 1024)   { v /= 1024.0 * 1024 * 1024;        unit = L"GB"; }
    else if (v >= 1024.0 * 1024)          { v /= 1024.0 * 1024;               unit = L"MB"; }
    else if (v >= 1024.0)                 { v /= 1024.0;                      unit = L"KB"; }
    swprintf(buf, 64, L"%llu (%.2f %ls)", static_cast<unsigned long long>(bytes), v, unit);
    return buf;
}

} // anonymous namespace

// ── CheckSsdSupport()-style key selection ─────────────────────────────

void SetSmartKeyName(DRIVE_INFO& asi) {
    const char* key = nullptr;

    if (asi.IsNVMe) {
        key = "SmartNVMe";
    } else if (asi.IsSsd) {
        key = VendorKey(asi.DiskVendorId);
        if (!key || !key[0]) key = "SmartSsd";
    } else {
        key = "Smart";
        // CrystalDiskInfo uses the vendor section for Seagate/WDC SSDs only;
        // rotating media always uses the generic [Smart] section.
    }

    asi.SmartKeyName = Widen(key);
    if (asi.SmartKeyName.empty()) asi.SmartKeyName = L"Smart";
}

// ── Attribute name lookup ─────────────────────────────────────────────

std::wstring GetSmartAttributeName(const DRIVE_INFO& asi, BYTE id) {
    if (id == 0) return L"Unknown";

    const char* key = asi.SmartKeyName.empty() ? "Smart" : nullptr;
    char narrowKey[64] = {};

    if (!key) {
        WideCharToMultiByte(CP_UTF8, 0, asi.SmartKeyName.c_str(), -1,
                            narrowKey, sizeof(narrowKey), nullptr, nullptr);
        key = narrowKey;
    }

    const CdiAttrName* entries = nullptr;
    int count = 0;

    if (CdiFindAttrTable(key, &entries, &count)) {
        for (int i = 0; i < count; ++i) {
            if (entries[i].id == id) return Widen(entries[i].name);
        }
    }

    // Fall back to the generic sections
    const char* fallback = asi.IsNVMe ? "SmartNVMe" : (asi.IsSsd ? "SmartSsd" : "Smart");
    if (strcmp(fallback, key) != 0 &&
        CdiFindAttrTable(fallback, &entries, &count)) {
        for (int i = 0; i < count; ++i) {
            if (entries[i].id == id) return Widen(entries[i].name);
        }
    }

    return L"Vendor Specific";
}

// ── Raw value presentation ────────────────────────────────────────────

std::wstring FormatSmartRawValue(const DRIVE_INFO& asi,
                                 const SMART_ATTRIBUTE& attr) {
    wchar_t buf[128] = {};

    // NOTE: detail::B8toB64le(const BYTE(&)[6]) is used for the raw field,
    // not cdi::B8toB64le(const BYTE*).  The latter reads EIGHT bytes, which
    // runs past SMART_ATTRIBUTE::RawValue[6] and picks up the following
    // attribute's Id — the RawValue field is only ever six bytes wide.
    // ── NVMe: attribute ids follow NVMeInterpreter's mapping ──────────
    if (asi.IsNVMe) {
        switch (attr.Id) {
        case 0x01: { // Critical Warning (bit field)
            if (attr.RawValue[0] == 0) return L"0 (Healthy)";
            std::wstring flags;
            if (attr.RawValue[0] & 0x01) flags += L"available_spare_low ";
            if (attr.RawValue[0] & 0x02) flags += L"temperature_high ";
            if (attr.RawValue[0] & 0x04) flags += L"reliability_degraded ";
            if (attr.RawValue[0] & 0x08) flags += L"read_only ";
            if (attr.RawValue[0] & 0x10) flags += L"volatile_memory_backup_failed ";
            if (attr.RawValue[0] & 0x20) flags += L"persistent_memory_read_only ";
            while (!flags.empty() && flags.back() == L' ') flags.pop_back();
            swprintf(buf, 128, L"%u (%ls)", attr.RawValue[0], flags.c_str());
            return buf;
        }
        case 0x02: { // Composite Temperature (Kelvin)
            WORD kelvin = B8toB16le(attr.RawValue);
            if (kelvin == 0) return L"--";
            swprintf(buf, 128, L"%u C (%u K)",
                     static_cast<unsigned>(kelvin > 273 ? kelvin - 273 : 0),
                     static_cast<unsigned>(kelvin));
            return buf;
        }
        case 0x03: // Available Spare
        case 0x04: // Available Spare Threshold
        case 0x05: // Percentage Used
            swprintf(buf, 128, L"%u %%", attr.RawValue[0]);
            return buf;
        case 0x06: // Data Units Read  (1 unit = 512 * 1000 bytes)
            return FormatBytes(detail::B8toB64le(attr.RawValue) * 512000ULL);
        case 0x07: // Data Units Written
            return FormatBytes(detail::B8toB64le(attr.RawValue) * 512000ULL);
        case 0x08: case 0x09: case 0x0A: case 0x0B: case 0x0D: case 0x0E: case 0x0F:
            swprintf(buf, 128, L"%llu",
                     static_cast<unsigned long long>(detail::B8toB64le(attr.RawValue)));
            return buf;
        case 0x0C: // Power On Hours
            swprintf(buf, 128, L"%llu h",
                     static_cast<unsigned long long>(detail::B8toB64le(attr.RawValue)));
            return buf;
        case 0x10: // Warning Composite Temperature Time (minutes)
        case 0x11: // Critical Composite Temperature Time (minutes)
            swprintf(buf, 128, L"%u min", B8toB32le(attr.RawValue));
            return buf;
        case 0x12: case 0x13: case 0x14: case 0x15:
        case 0x16: case 0x17: case 0x18: case 0x19: { // Temperature Sensors 1-8
            WORD kelvin = B8toB16le(attr.RawValue);
            if (kelvin == 0) return L"--";
            swprintf(buf, 128, L"%u C", static_cast<unsigned>(kelvin > 273 ? kelvin - 273 : 0));
            return buf;
        }
        case 0x1A: case 0x1B: case 0x1C: case 0x1D: // Thermal management counters
            swprintf(buf, 128, L"%u", B8toB32le(attr.RawValue));
            return buf;
        default:
            break;
        }

        // Default NVMe presentation: 12-digit hexadecimal raw dump
        swprintf(buf, 128, L"%02X%02X%02X%02X%02X%02X",
                 attr.RawValue[5], attr.RawValue[4], attr.RawValue[3],
                 attr.RawValue[2], attr.RawValue[1], attr.RawValue[0]);
        return buf;
    }

    // ── Power-On Hours ────────────────────────────────────────────────
    if (attr.Id == 0x09) {
        DWORD hours = CAtaSmart::GetPowerOnHours(B8toB32le(attr.RawValue),
                                                 asi.MeasuredTimeUnitType);
        swprintf(buf, 128, L"%u h", hours);
        return buf;
    }

    // ── Power Cycle Count ─────────────────────────────────────────────
    if (attr.Id == 0x0C) {
        swprintf(buf, 128, L"%u", B8toB32le(attr.RawValue));
        return buf;
    }

    // ── Temperature ───────────────────────────────────────────────────
    if (attr.Id == 0xC2 || attr.Id == 0xBE || attr.Id == 0xE7 || attr.Id == 0xF3) {
        int t = ExtractTemperature(asi, attr.Id, attr);
        if (t == -1000 || t <= 0) {
            swprintf(buf, 128, L"%02X%02X%02X%02X%02X%02X",
                     attr.RawValue[5], attr.RawValue[4], attr.RawValue[3],
                     attr.RawValue[2], attr.RawValue[1], attr.RawValue[0]);
            return buf;
        }
        swprintf(buf, 128, L"%d C", t);
        return buf;
    }

    // ── Remaining life style attributes ───────────────────────────────
    if (IsLifeAttribute(asi, attr.Id)) {
        int life = attr.CurrentValue;
        if (attr.Id == 0xE6 &&
            (asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SANDISK)) {
            life = 100 - attr.RawValue[1];
        } else if (attr.Id == 0xE8 && asi.DiskVendorId == SSD_VENDOR_PLEXTOR) {
            life = attr.RawValue[0];
        }
        if (life < 0) life = 0;
        if (life > 100) life = 100;
        swprintf(buf, 128, L"%d %%", life);
        return buf;
    }

    // ── Sector counts: 16-bit little endian ───────────────────────────
    switch (attr.Id) {
    case 0x05: // Reallocated Sectors Count
    case 0xC4: // Reallocation Event Count
    case 0xC5: // Current Pending Sector Count
    case 0xC6: // Uncorrectable Sector Count
        swprintf(buf, 128, L"%llu",
                 static_cast<unsigned long long>(detail::B8toB64le(attr.RawValue)));
        return buf;
    default:
        break;
    }

    // ── Default: 48-bit little endian decimal dump ────────────────────
    swprintf(buf, 128, L"%llu",
             static_cast<unsigned long long>(RawLe48(attr)));
    return buf;
}

// ── Attribute index lookup ────────────────────────────────────────────

int GetSmartAttributeIndex(const DRIVE_INFO& asi, BYTE id) {
    for (DWORD i = 0; i < asi.AttributeCount && i < MAX_ATTRIBUTE; ++i) {
        if (asi.Attribute[i].Id == id) return static_cast<int>(i);
    }
    return -1;
}

} // namespace cdi
