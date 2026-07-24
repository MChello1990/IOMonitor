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

#include "SmartDataModel.h"
#include <cstring>
#include <cstdio>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════
//  Default attribute definitions — exact replica of smartmontools'
//  DEFAULT entry in drivedb.h (7.5+)
//  Format: {name, raw_format, flags, byteorder}
// ═══════════════════════════════════════════════════════════════════════

const AttrDefEntry g_defaultAttrDefs[MAX_ATTRIBUTE_NUM] = {
    // 0 — not used
    {},
    // 1
    { "Raw_Read_Error_Rate",          AttrRawFormat::RAWFMT_RAW48, 0, "543210" },
    // 2
    { "Throughput_Performance",       AttrRawFormat::RAWFMT_RAW48, 0, "543210" },
    // 3
    { "Spin_Up_Time",                 AttrRawFormat::RAWFMT_RAW16_OPT_AVG16, 0, "543210" },
    // 4
    { "Start_Stop_Count",             AttrRawFormat::RAWFMT_RAW48, 0, "543210" },
    // 5
    { "Reallocated_Sector_Ct",        AttrRawFormat::RAWFMT_RAW16_OPT_RAW16, 0, "543210" },
    // 6
    { "Read_Channel_Margin",          AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 7
    { "Seek_Error_Rate",              AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 8
    { "Seek_Time_Performance",        AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 9
    { "Power_On_Hours",               AttrRawFormat::RAWFMT_RAW24_OPT_RAW8, 0, "543210" },
    // 10
    { "Spin_Retry_Count",             AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 11
    { "Calibration_Retry_Count",      AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 12
    { "Power_Cycle_Count",            AttrRawFormat::RAWFMT_RAW48, 0, "543210" },
    // 13
    { "Read_Soft_Error_Rate",         AttrRawFormat::RAWFMT_RAW48, 0, "543210" },
    // 14-21 — Unknown
    {}, {}, {}, {}, {}, {}, {}, {},
    // 22
    { "Helium_Level",                 AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 23
    { "Helium_Condition_Lower",       AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 24
    { "Helium_Condition_Upper",       AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210" },
    // 25-174 — Unknown (will fill SSD-specific below)
};

// Static initializer fills remaining entries
namespace {
    struct AttrDefsInitializer {
        AttrDefsInitializer() {
            // Copy base 0-24 entries into g_defaultAttrDefs
            // (already done via constexpr initialization for first 25)
            // Fill remaining entries

            // 175
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[175]) = {
                "Program_Fail_Count_Chip", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 176
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[176]) = {
                "Erase_Fail_Count_Chip", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 177
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[177]) = {
                "Wear_Leveling_Count", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 178
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[178]) = {
                "Used_Rsvd_Blk_Cnt_Chip", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 179
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[179]) = {
                "Used_Rsvd_Blk_Cnt_Tot", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 180
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[180]) = {
                "Unused_Rsvd_Blk_Cnt_Tot", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 181
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[181]) = {
                "Program_Fail_Cnt_Total", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 182
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[182]) = {
                "Erase_Fail_Count_Total", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 183
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[183]) = {
                "Runtime_Bad_Block", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 184
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[184]) = {
                "End-to-End_Error", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 187
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[187]) = {
                "Reported_Uncorrect", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 188
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[188]) = {
                "Command_Timeout", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 189
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[189]) = {
                "High_Fly_Writes", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 190
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[190]) = {
                "Airflow_Temperature_Cel", AttrRawFormat::RAWFMT_TEMPMINMAX, 0, "543210"
            };
            // 191
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[191]) = {
                "G-Sense_Error_Rate", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 192
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[192]) = {
                "Power-Off_Retract_Count", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 193
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[193]) = {
                "Load_Cycle_Count", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 194
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[194]) = {
                "Temperature_Celsius", AttrRawFormat::RAWFMT_TEMPMINMAX, 0, "543210"
            };
            // 195
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[195]) = {
                "Hardware_ECC_Recovered", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 196
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[196]) = {
                "Reallocated_Event_Count", AttrRawFormat::RAWFMT_RAW16_OPT_RAW16, 0, "543210"
            };
            // 197
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[197]) = {
                "Current_Pending_Sector", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 198
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[198]) = {
                "Offline_Uncorrectable", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 199
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[199]) = {
                "UDMA_CRC_Error_Count", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 200
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[200]) = {
                "Multi_Zone_Error_Rate", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 201
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[201]) = {
                "Soft_Read_Error_Rate", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 202
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[202]) = {
                "Data_Address_Mark_Errs", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 203
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[203]) = {
                "Run_Out_Cancel", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 204
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[204]) = {
                "Soft_ECC_Correction", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 205
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[205]) = {
                "Thermal_Asperity_Rate", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 206
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[206]) = {
                "Flying_Height", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 207
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[207]) = {
                "Spin_High_Current", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 208
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[208]) = {
                "Spin_Buzz", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 209
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[209]) = {
                "Offline_Seek_Performnce", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 220
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[220]) = {
                "Disk_Shift", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 221
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[221]) = {
                "G-Sense_Error_Rate", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 222
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[222]) = {
                "Loaded_Hours", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 223
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[223]) = {
                "Load_Retry_Count", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 224
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[224]) = {
                "Load_Friction", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 225
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[225]) = {
                "Load_Cycle_Count", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 226
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[226]) = {
                "Load-in_Time", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 227
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[227]) = {
                "Torq-amp_Count", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 228
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[228]) = {
                "Power-off_Retract_Count", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 230
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[230]) = {
                "Head_Amplitude", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 231
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[231]) = {
                "Temperature_Celsius", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 232
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[232]) = {
                "Available_Reservd_Space", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 233
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[233]) = {
                "Media_Wearout_Indicator", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_SSD_ONLY, "543210"
            };
            // 240
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[240]) = {
                "Head_Flying_Hours", AttrRawFormat::RAWFMT_RAW24_OPT_RAW8, ATTRFLAG_HDD_ONLY, "543210"
            };
            // 241
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[241]) = {
                "Total_LBAs_Written", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 242
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[242]) = {
                "Total_LBAs_Read", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 250
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[250]) = {
                "Read_Error_Retry_Rate", AttrRawFormat::RAWFMT_RAW48, 0, "543210"
            };
            // 254
            const_cast<AttrDefEntry&>(g_defaultAttrDefs[254]) = {
                "Free_Fall_Sensor", AttrRawFormat::RAWFMT_RAW48, ATTRFLAG_HDD_ONLY, "543210"
            };
        }
    } _init;
}

// ═══════════════════════════════════════════════════════════════════════
//  Checksum (smartmontools' checksum() exact replica)
// ═══════════════════════════════════════════════════════════════════════

unsigned char checksum(const void* data) {
    unsigned char sum = 0;
    for (int i = 0; i < 512; i++)
        sum += ((const unsigned char*)data)[i];
    return sum;
}

// ═══════════════════════════════════════════════════════════════════════
//  ATA string extraction (byte-swapped, per ATA spec)
// ═══════════════════════════════════════════════════════════════════════

std::string extractAtaString(const uint8_t* buf, size_t len) {
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; i += 2) {
        if (i + 1 < len) {
            char c1 = static_cast<char>(buf[i + 1]);
            char c2 = static_cast<char>(buf[i]);
            if (c1 != 0) result.push_back(c1);
            if (c2 != 0) result.push_back(c2);
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::wstring ataToWide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &result[0], len);
    return result;
}

// ═══════════════════════════════════════════════════════════════════════
//  Get raw value — smartmontools' ata_get_attr_raw_value() exact replica
// ═══════════════════════════════════════════════════════════════════════

uint64_t ataGetAttrRawValue(const AtaSmartAttribute& attr, const AttrDefEntry& def) {
    const char* byteorder = def.byteorder;
    if (!byteorder[0]) byteorder = "543210";

    uint64_t rawvalue = 0;
    for (int i = 0; byteorder[i]; i++) {
        unsigned char b = 0;
        switch (byteorder[i]) {
            case '0': b = attr.raw[0]; break;
            case '1': b = attr.raw[1]; break;
            case '2': b = attr.raw[2]; break;
            case '3': b = attr.raw[3]; break;
            case '4': b = attr.raw[4]; break;
            case '5': b = attr.raw[5]; break;
            case 'r': b = attr.reserv; break;
            case 'v': b = attr.current; break;
            case 'w': b = attr.worst; break;
            default: break;
        }
        rawvalue <<= 8;
        rawvalue |= b;
    }
    return rawvalue;
}

// ═══════════════════════════════════════════════════════════════════════
//  Format raw value — smartmontools' ata_format_attr_raw_value()
// ═══════════════════════════════════════════════════════════════════════

static inline bool check_temp_range(int t, uint8_t ut1, uint8_t ut2, int& lo, int& hi) {
    int t1 = (signed char)ut1, t2 = (signed char)ut2;
    if (t1 > t2) { int tx = t1; t1 = t2; t2 = tx; }
    if (-60 <= t1 && t1 <= t && t <= t2 && t2 <= 120 && !(t1 == -1 && t2 <= 0)) {
        lo = t1; hi = t2; return true;
    }
    return false;
}

static inline int check_temp_word(uint16_t word) {
    if (word <= 0x7f) return 0x11;
    if (word <= 0xff) return 0x01;
    if (0xff80 <= word) return 0x10;
    return 0x00;
}

std::string ataFormatAttrRawValue(const AtaSmartAttribute& attr, const AttrDefEntry& def) {
    uint64_t rawvalue = ataGetAttrRawValue(attr, def);

    unsigned char raw[6];
    raw[0] = (unsigned char) rawvalue;
    raw[1] = (unsigned char)(rawvalue >> 8);
    raw[2] = (unsigned char)(rawvalue >> 16);
    raw[3] = (unsigned char)(rawvalue >> 24);
    raw[4] = (unsigned char)(rawvalue >> 32);
    raw[5] = (unsigned char)(rawvalue >> 40);

    uint16_t word[3];
    word[0] = raw[0] | (raw[1] << 8);
    word[1] = raw[2] | (raw[3] << 8);
    word[2] = raw[4] | (raw[5] << 8);

    AttrRawFormat format = def.raw_format;
    if (format == AttrRawFormat::RAWFMT_DEFAULT)
        format = AttrRawFormat::RAWFMT_RAW48;

    char buf[256] = {};

    switch (format) {
    case AttrRawFormat::RAWFMT_RAW8:
        snprintf(buf, sizeof(buf), "%d %d %d %d %d %d", raw[5], raw[4], raw[3], raw[2], raw[1], raw[0]);
        break;
    case AttrRawFormat::RAWFMT_RAW16:
        snprintf(buf, sizeof(buf), "%u %u %u", word[2], word[1], word[0]);
        break;
    case AttrRawFormat::RAWFMT_RAW48:
    case AttrRawFormat::RAWFMT_RAW56:
    case AttrRawFormat::RAWFMT_RAW64:
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)rawvalue);
        break;
    case AttrRawFormat::RAWFMT_HEX48:
        snprintf(buf, sizeof(buf), "0x%012llx", (unsigned long long)(rawvalue & 0xFFFFFFFFFFFFULL));
        break;
    case AttrRawFormat::RAWFMT_HEX56:
        snprintf(buf, sizeof(buf), "0x%014llx", (unsigned long long)(rawvalue & 0xFFFFFFFFFFFFFFULL));
        break;
    case AttrRawFormat::RAWFMT_HEX64:
        snprintf(buf, sizeof(buf), "0x%016llx", (unsigned long long)rawvalue);
        break;
    case AttrRawFormat::RAWFMT_RAW16_OPT_RAW16:
        snprintf(buf, sizeof(buf), "%u", word[0]);
        if (word[1] || word[2])
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (%u %u)", word[2], word[1]);
        break;
    case AttrRawFormat::RAWFMT_RAW16_OPT_AVG16:
        snprintf(buf, sizeof(buf), "%u", word[0]);
        if (word[1])
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (Average %u)", word[1]);
        break;
    case AttrRawFormat::RAWFMT_RAW24_OPT_RAW8:
        snprintf(buf, sizeof(buf), "%u", (unsigned)(rawvalue & 0xFFFFFFULL));
        if (raw[3] || raw[4] || raw[5])
            snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), " (%d %d %d)", raw[5], raw[4], raw[3]);
        break;
    case AttrRawFormat::RAWFMT_RAW24_DIV_RAW24:
        snprintf(buf, sizeof(buf), "%u/%u",
                 (unsigned)(rawvalue >> 24), (unsigned)(rawvalue & 0xFFFFFFULL));
        break;
    case AttrRawFormat::RAWFMT_RAW24_DIV_RAW32:
        snprintf(buf, sizeof(buf), "%u/%u",
                 (unsigned)(rawvalue >> 32), (unsigned)(rawvalue & 0xFFFFFFFFULL));
        break;
    case AttrRawFormat::RAWFMT_MIN2HOUR: {
        int64_t temp = word[0] + ((int64_t)word[1] << 16);
        snprintf(buf, sizeof(buf), "%lldh+%02lldm",
                 (long long)(temp / 60), (long long)(temp % 60));
        break;
    }
    case AttrRawFormat::RAWFMT_SEC2HOUR: {
        int64_t hours = (int64_t)rawvalue / 3600;
        int64_t minutes = ((int64_t)rawvalue - 3600 * hours) / 60;
        int64_t seconds = (int64_t)rawvalue % 60;
        snprintf(buf, sizeof(buf), "%lldh+%02lldm+%02llds",
                 (long long)hours, (long long)minutes, (long long)seconds);
        break;
    }
    case AttrRawFormat::RAWFMT_HALFMIN2HOUR: {
        int64_t hours = (int64_t)rawvalue / 120;
        int64_t minutes = ((int64_t)rawvalue - 120 * hours) / 2;
        snprintf(buf, sizeof(buf), "%lldh+%02lldm", (long long)hours, (long long)minutes);
        break;
    }
    case AttrRawFormat::RAWFMT_MSEC24_HOUR32: {
        unsigned hours = (unsigned)(rawvalue & 0xFFFFFFFFULL);
        unsigned milliseconds = (unsigned)(rawvalue >> 32);
        unsigned seconds = milliseconds / 1000;
        snprintf(buf, sizeof(buf), "%uh+%02um+%02u.%03us",
                 hours, seconds / 60, seconds % 60, milliseconds % 1000);
        break;
    }
    case AttrRawFormat::RAWFMT_TEMPMINMAX: {
        int t = (signed char)raw[0];
        int lo = 0, hi = 0;
        int tformat;
        int ctw0 = check_temp_word(word[0]);
        if (!word[2]) {
            if (!word[1] && ctw0)
                tformat = 0;
            else if (ctw0 && check_temp_range(t, raw[2], raw[3], lo, hi))
                tformat = 1;
            else if (!raw[3] && check_temp_range(t, raw[1], raw[2], lo, hi))
                tformat = 2;
            else
                tformat = -1;
        } else if (ctw0) {
            if ((ctw0 & check_temp_word(word[1]) & check_temp_word(word[2])) != 0
                && check_temp_range(t, raw[2], raw[4], lo, hi))
                tformat = 3;
            else if (word[2] < 0x7fff && check_temp_range(t, raw[2], raw[3], lo, hi) && hi >= 40)
                tformat = 4;
            else
                tformat = -2;
        } else {
            tformat = -3;
        }
        switch (tformat) {
            case 0: snprintf(buf, sizeof(buf), "%d", t); break;
            case 1: case 2: case 3:
                snprintf(buf, sizeof(buf), "%d (Min/Max %d/%d)", t, lo, hi); break;
            case 4:
                snprintf(buf, sizeof(buf), "%d (Min/Max %d/%d #%d)", t, lo, hi, word[2]); break;
            default:
                snprintf(buf, sizeof(buf), "%d (%d %d %d %d %d)", raw[0], raw[5], raw[4], raw[3], raw[2], raw[1]);
                break;
        }
        break;
    }
    case AttrRawFormat::RAWFMT_TEMP10X:
        snprintf(buf, sizeof(buf), "%d.%d", word[0] / 10, word[0] % 10);
        break;
    default:
        snprintf(buf, sizeof(buf), "%llu", (unsigned long long)rawvalue);
        break;
    }
    return buf;
}

// ═══════════════════════════════════════════════════════════════════════
//  Get attribute name — smartmontools' ata_get_smart_attr_name()
// ═══════════════════════════════════════════════════════════════════════

std::string ataGetSmartAttrName(uint8_t id, int rpm /* = 0 */) {
    if (id >= MAX_ATTRIBUTE_NUM) return "Unknown_Attribute";
    const auto& def = g_defaultAttrDefs[id];
    if (def.hasName()) {
        if ((def.flags & ATTRFLAG_HDD_ONLY) && rpm == 1)
            return "Unknown_SSD_Attribute";
        if ((def.flags & ATTRFLAG_SSD_ONLY) && rpm > 1)
            return "Unknown_HDD_Attribute";
        return def.name;
    }
    return "Unknown_Attribute";
}

// ═══════════════════════════════════════════════════════════════════════
//  Get attribute state — smartmontools' ata_get_attr_state()
// ═══════════════════════════════════════════════════════════════════════

AttrState ataGetAttrState(const AtaSmartAttribute& attr, int attridx,
                          const AtaSmartThresholdEntry* thresholds,
                          uint8_t* threshval /* = nullptr */) {
    if (!attr.id)
        return AttrState::NON_EXISTING;

    const auto& def = g_defaultAttrDefs[attr.id];
    if (def.flags & ATTRFLAG_NO_NORMVAL)
        return AttrState::NO_NORMVAL;

    int i = attridx;
    if (thresholds[i].id != attr.id) {
        for (i = 0; thresholds[i].id != attr.id; ) {
            if (++i >= NUMBER_ATA_SMART_ATTRIBUTES)
                return AttrState::NO_THRESHOLD;
        }
    }

    uint8_t threshold = thresholds[i].threshold;
    if (threshval) *threshval = threshold;

    if (!threshold)
        return AttrState::OK;

    if (attr.current <= threshold)
        return AttrState::FAILED_NOW;

    if (!(def.flags & ATTRFLAG_NO_WORSTVAL) && attr.worst <= threshold)
        return AttrState::FAILED_PAST;

    return AttrState::OK;
}

// ═══════════════════════════════════════════════════════════════════════
//  Find attribute index — smartmontools' ata_find_attr_index()
// ═══════════════════════════════════════════════════════════════════════

int ataFindAttrIndex(uint8_t id, const AtaSmartValues& smartval) {
    if (!id) return -1;
    for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; i++) {
        if (smartval.vendor_attributes[i].id == id)
            return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════════
//  Return temperature value — smartmontools' ata_return_temperature_value()
// ═══════════════════════════════════════════════════════════════════════

uint8_t ataReturnTemperatureValue(const AtaSmartValues* data) {
    static const uint8_t ids[4] = { 194, 190, 9, 220 };
    for (int i = 0; i < 4; i++) {
        uint8_t id = ids[i];
        const auto& def = g_defaultAttrDefs[id];
        AttrRawFormat format = def.raw_format;
        if (!( ((id == 194 || id == 190) && format == AttrRawFormat::RAWFMT_DEFAULT)
               || format == AttrRawFormat::RAWFMT_TEMPMINMAX
               || format == AttrRawFormat::RAWFMT_TEMP10X))
            continue;

        int idx = ataFindAttrIndex(id, *data);
        if (idx < 0) continue;

        uint64_t raw = ataGetAttrRawValue(data->vendor_attributes[idx], def);
        unsigned temp;
        if (format == AttrRawFormat::RAWFMT_TEMP10X)
            temp = ((uint16_t)raw + 5) / 10;
        else
            temp = (uint8_t)raw;

        if (0 < temp && temp < 128)
            return (uint8_t)temp;
    }
    return 0;
}

// ═══════════════════════════════════════════════════════════════════════
//  Read 128-bit LE value from buffer (for NVMe)
// ═══════════════════════════════════════════════════════════════════════

uint64_t readUint128le(const uint8_t* p) {
    uint64_t lo = 0;
    for (int i = 0; i < 8; ++i)
        lo |= static_cast<uint64_t>(p[i]) << (i * 8);
    return lo;
}
