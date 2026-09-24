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
//  CdiSmartSupport — definitions for the declarations in CdiSmartDetail.h
//
//  Holds everything the CdiSmart*.cpp translation units share and that has
//  to live in exactly one of them:
//
//    * CrystalDiskInfo AtaSmart.h's file-static string tables
//      (commandTypeString / ssdVendorString / deviceFormFactorString)
//    * the FlagLife* / NandWritesUnit side channel (see CdiSmartDetail.h)
//    * CAtaSmart::CheckSmartAttributeCorrect
//
//  Upstream references are to <https://github.com/hiyohiyo/CrystalDiskInfo>
//  (Copyright (c) hiyohiyo, MIT License).
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmartDetail.h"

#include <cwchar>

namespace cdi {
namespace detail {

// ── commandTypeString — CrystalDiskInfo AtaSmart.h:23-56 ──────────────
//
// Upstream lists 33 entries because it has 33 COMMAND_TYPE values.  This
// port keeps only the 8 declared in CdiSmart.h, in enum order, so the
// indices stay aligned with CMD_TYPE_*.
const wchar_t* const kCommandTypeString[8] = {
    L"un",   // CMD_TYPE_UNKNOWN
    L"pd",   // CMD_TYPE_PHYSICAL_DRIVE
    L"sm",   // CMD_TYPE_SCSI_MINIPORT
    L"sa",   // CMD_TYPE_SAT
    L"jm",   // CMD_TYPE_JMICRON
    L"sq",   // CMD_TYPE_NVME_STORAGE_QUERY
    L"ni",   // CMD_TYPE_NVME_INTEL
    L"dg",   // CMD_TYPE_DEBUG
};

// ── ssdVendorString — CrystalDiskInfo AtaSmart.h:60-110 ───────────────
//
// Verbatim, including the empty entries.  Indexed by VENDOR_ID, which in
// CdiSmart.h is the same enum in the same order as upstream.
const wchar_t* const kSsdVendorString[SSD_VENDOR_STRING_COUNT] = {
    L"",      //   0 HDD_GENERAL
    L"",      //   1 SSD_GENERAL
    L"mt",    //   2 SSD_VENDOR_MTRON
    L"ix",    //   3 SSD_VENDOR_INDILINX
    L"jm",    //   4 SSD_VENDOR_JMICRON
    L"il",    //   5 SSD_VENDOR_INTEL
    L"sg",    //   6 SSD_VENDOR_SAMSUNG
    L"sf",    //   7 SSD_VENDOR_SANDFORCE
    L"mi",    //   8 SSD_VENDOR_MICRON
    L"oz",    //   9 SSD_VENDOR_OCZ
    L"st",    //  10 SSD_VENDOR_SEAGATE
    L"wd",    //  11 SSD_VENDOR_WDC
    L"px",    //  12 SSD_VENDOR_PLEXTOR
    L"sd",    //  13 SSD_VENDOR_SANDISK
    L"oz",    //  14 SSD_VENDOR_OCZ_VECTOR
    L"to",    //  15 SSD_VENDOR_TOSHIBA       (upstream comment says "TOSHIABA")
    L"co",    //  16 SSD_VENDOR_CORSAIR
    L"ki",    //  17 SSD_VENDOR_KINGSTON
    L"m3",    //  18 SSD_VENDOR_MICRON_MU03
    L"nv",    //  19 SSD_VENDOR_NVME
    L"re",    //  20 SSD_VENDOR_REALTEK
    L"sk",    //  21 SSD_VENDOR_SKHYNIX
    L"ki",    //  22 SSD_VENDOR_KIOXIA
    L"ss",    //  23 SSD_VENDOR_SSSTC
    L"id",    //  24 SSD_VENDOR_INTEL_DC
    L"ap",    //  25 SSD_VENDOR_APACER
    L"sm",    //  26 SSD_VENDOR_SILICONMOTION
    L"ph",    //  27 SSD_VENDOR_PHISON
    L"ma",    //  28 SSD_VENDOR_MARVELL
    L"mk",    //  29 SSD_VENDOR_MAXIOTEK
    L"ym",    //  30 SSD_VENDOR_YMTC
    L"sc",    //  31 SSD_VENDOR_SCY
    L"",      //  32 SSD_VENDOR_JMICRON_60X
    L"",      //  33 SSD_VENDOR_JMICRON_61X
    L"",      //  34 SSD_VENDOR_JMICRON_66X
    L"",      //  35 SSD_VENDOR_SEAGATE_IRON_WOLF
    L"",      //  36 SSD_VENDOR_SEAGATE_BARRA_CUDA
    L"",      //  37 SSD_VENDOR_SANDISK_GB
    L"",      //  38 SSD_VENDOR_KINGSTON_SUV
    L"",      //  39 SSD_VENDOR_KINGSTON_KC600
    L"",      //  40 SSD_VENDOR_KINGSTON_DC500
    L"",      //  41 SSD_VENDOR_KINGSTON_SA400
    L"re",    //  42 SSD_VENDOR_RECADATA
    L"",      //  43 SSD_VENDOR_SANDISK_DELL
    L"",      //  44 SSD_VENDOR_SANDISK_HP
    L"",      //  45 SSD_VENDOR_SANDISK_HP_VENUS
    L"",      //  46 SSD_VENDOR_SANDISK_LENOVO
    L"",      //  47 SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS
    L"",      //  48 SSD_VENDOR_SANDISK_CLOUD
    L"mc",    //  49 SSD_VENDOR_SILICONMOTION_CVC
    L"ai",    //  50 SSD_VENDOR_ADATA_INDUSTRIAL
};

// ── deviceFormFactorString — CrystalDiskInfo AtaSmart.h:171-179 ───────
const wchar_t* const kDeviceFormFactorString[6] = {
    L"",
    L"5.25 inch",
    L"3.5 inch",
    L"2.5 inch",
    L"1.8 inch",
    L"< 1.8 inch",
};

std::wstring DeviceFormFactorString(WORD word168) {
    // CrystalDiskInfo AddDisk() (AtaSmart.cpp:3082-3092) indexes the table
    // with the low nibble of word 168 — bits 0-3 are the nominal form factor
    // (1 = 5.25", 2 = 3.5", 3 = 2.5", 4 = 1.8", 5 = < 1.8") — and guards it
    // with "(w168 & 0xF) > 0 && <= 5".  Clamping instead of switching keeps
    // the same observable result and cannot index out of bounds.
    const WORD index = static_cast<WORD>(word168 & 0x0F);
    if (index >= 6) return std::wstring();
    return kDeviceFormFactorString[index];
}

// ── FlagLife* / NandWritesUnit side channel ───────────────────────────

namespace {

// Function-local static so every translation unit that includes
// CdiSmartDetail.h sees the SAME store without needing an extern definition.
std::map<std::wstring, SsdLifeCtx>& Store() {
    static std::map<std::wstring, SsdLifeCtx> store;
    return store;
}

} // anonymous namespace

std::wstring LifeCtxKey(const DRIVE_INFO& asi) {
    // Model + serial uniquely identifies a physical drive, and both are
    // already corrected for byte order by the time CheckSsdSupport runs.
    // The 0x1F separator cannot occur in an ATA identify string.
    return asi.Model + L'\x1f' + asi.SerialNumber;
}

SsdLifeCtx& LifeCtx(DRIVE_INFO& asi) {
    return Store()[LifeCtxKey(asi)];
}

SsdLifeCtx LifeCtxOf(const DRIVE_INFO& asi) {
    const auto it = Store().find(LifeCtxKey(asi));
    return it == Store().end() ? SsdLifeCtx{} : it->second;
}

void ClearLifeCtx() {
    Store().clear();
}

// ── CheckSmartAttributeCorrect — CrystalDiskInfo AtaSmart.cpp:6392-6410 ─

BOOL CheckSmartAttributeCorrect(const DRIVE_INFO* asi1, const DRIVE_INFO* asi2) {
    if (asi1 == nullptr || asi2 == nullptr) return FALSE;
    if (asi1->AttributeCount != asi2->AttributeCount) return FALSE;

    for (DWORD i = 0; i < asi1->AttributeCount && i < MAX_ATTRIBUTE; ++i) {
        if (asi1->Attribute[i].Id != asi2->Attribute[i].Id) return FALSE;
    }
    return TRUE;
}

} // namespace detail
} // namespace cdi
