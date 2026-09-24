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
//  CdiSmartSsd — SSD vendor detection
//
//  Ported from CrystalDiskInfo <https://github.com/hiyohiyo/CrystalDiskInfo>
//  (Copyright (c) hiyohiyo, MIT License).  This unit covers
//
//    * CAtaSmart::CheckSsdSupport() ......... AtaSmart.cpp:4149-4982
//    * CAtaSmart::IsSamsungEnterpriseModel()  AtaSmart.cpp:5204-5226
//    * the IsSsdXxx() predicate family ...... AtaSmart.cpp:4985-6390
//      IsSsdOld            5017   IsSsdOczVector          5456
//      IsSsdMtron          5039   IsSsdSsstc              5524
//      IsSsdJMicron60x     5044   IsSsdPlextor            5536
//      IsSsdJMicron61x     5064   IsSsdSanDisk            5562
//      IsSsdJMicron66x     5092   IsSsdKingston           5701
//      IsSsdIndilinx       5123   IsSsdCorsair            5757
//      IsSsdIntelDc        5165   IsSsdToshiba            5773
//      IsSsdIntel          5171   IsSsdRealtek            5802
//      IsSsdSamsung        5228   IsSsdSKhynix            5826
//      IsSsdSandForce      5298   IsSsdKioxia             5862
//      IsSsdMicronMU03     5353   IsSsdApacer             5874
//      IsSsdMicron         5388   IsSsdYmtc               5892
//      IsSsdOcz            5425   IsSsdScy                5904
//      IsSsdRecadata       5916   IsSsdMarvell            6247
//      IsSsdSiliconMotionCVC 5928 IsSsdMaxiotek           6298
//      IsSsdSiliconMotion  5940   IsSsdAdataIndustrial    6343
//      IsSsdPhison         6085   IsSsdGeneral            6366
//      IsSsdWdc            6144
//      IsSsdSeagate        6165
//
//  ── Side channel ──────────────────────────────────────────────────────
//
//  CrystalDiskInfo keeps nine policy booleans on ATA_SMART_INFO that the
//  IsSsdXxx helpers set and CheckSsdSupport() / FillSmartData() /
//  CheckDiskStatus() read.  DRIVE_INFO does not carry them, so they live
//  in cdi::detail::SsdLifeCtx, keyed on Model + SerialNumber (see the
//  long comment in CdiSmartDetail.h).  One context is fetched here with
//  detail::LifeCtx(asi) and the reference is threaded into every helper:
//
//      asi.FlagLifeNoReport          -> life.FlagLifeNoReport
//      asi.FlagLifeRawValue          -> life.FlagLifeRawValue
//      asi.FlagLifeRawValueIncrement -> life.FlagLifeRawValueIncrement
//      asi.FlagLifeSanDiskUsbMemory  -> life.FlagLifeSanDiskUsbMemory
//      asi.FlagLifeSanDisk0_1        -> life.FlagLifeSanDisk0_1
//      asi.FlagLifeSanDisk1          -> life.FlagLifeSanDisk1
//      asi.FlagLifeSanDiskLenovo     -> life.FlagLifeSanDiskLenovo
//      asi.FlagLifeSanDiskCloud      -> life.FlagLifeSanDiskCloud
//      asi.NandWritesUnit == NAND_WRITES_1MB -> life.NandWrites1MB = TRUE
//
//  Two more upstream booleans have no home at all:
//
//      asi.IsRawValues8  (set by IsSsdJMicron60x and IsSsdIndilinx)
//      asi.IsRawValues7  (set by IsSsdSandForce)
//
//  Upstream keeps them *in parallel* with asi.SmartKeyName, which those
//  three check points assign a distinct value at the same moment, so the
//  port folds both flags into SmartKeyName alone: the helpers set
//  L"SmartJMicron60x" / L"SmartIndilinx" / L"SmartSandForce" and no flag
//  is stored.  Every consumer derives the flag from SmartKeyName — see
//  the IsRawValues8 / IsRawValues7 comment in CdiSmartDetail.h.
//
//  ── attributeString[] ─────────────────────────────────────────────────
//
//  Upstream AtaSmart.h:116-169 keeps attributeString[] indexed by
//  DiskVendorId.  AtaSmart.cpp never indexes it (CheckSsdSupport writes
//  the same literals inline); the table is reproduced here so the
//  DiskVendorId -> SmartKeyName relation stays in one place, and it is
//  used for the two terminal branches where the literal and the table
//  entry are provably identical (HDD_GENERAL / SSD_GENERAL).
//
//  Note for CdiAttributeName.cpp: cdi::SetSmartKeyName() is a *fallback*.
//  This function is what CrystalDiskInfo's classification produces, so it
//  must win.  Calling SetSmartKeyName() *after* CheckSsdSupport() would
//  overwrite SmartKeyName and silently break the IsRawValues7/8 folding
//  above (and the FlagLife* side channel with it).
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmartDetail.h"

#include <string>

namespace cdi {

// The ported helpers and this file's small string tables live in an
// anonymous namespace; the names imported below keep the bodies textually
// close to upstream (which uses CString vocabulary).
//
// detail::B8toB64le is deliberately NOT imported: CdiSmart.h declares
// cdi::B8toB64le(const BYTE*) as well, and with both in scope a 6 byte
// SMART raw field would bind equally well to either (C2668), so every
// call site qualifies it instead.
using detail::B8toINTle;
using detail::Contains;
using detail::ContainsAfter0;
using detail::IsSamsungEnterpriseModel;
using detail::kSsdVendorString;
using detail::LifeCtx;
using detail::SSD_VENDOR_STRING_COUNT;
using detail::SsdLifeCtx;
using detail::StartsWith;
using detail::ToUpperCopy;

namespace detail {

// ── AtaSmart.cpp:5204-5226 ────────────────────────────────────────────
//
// Detect Samsung enterprise SATA models by model string.  Upstream takes a
// CString (their comment: "Using CString keeps us header-agnostic"); the
// port takes a const std::wstring& as declared in CdiSmartDetail.h.

BOOL IsSamsungEnterpriseModel(const std::wstring& model)
{
    std::wstring mu = ToUpperCopy(model);   // upstream: CString mu = model; mu.MakeUpper();

    // Retail/line names
    if (Contains(mu, L"SM863") || Contains(mu, L"PM863") || Contains(mu, L"PM863A")
        || Contains(mu, L"SM883") || Contains(mu, L"PM883") || Contains(mu, L"SM843T")
        || Contains(mu, L"PM853T"))
    {
        return TRUE;
    }

    // Common OEM codes for those lines (e.g., your MZ7KM... SM863)
    if (Contains(mu, L"MZ7KM") || Contains(mu, L"MZ7KH") || Contains(mu, L"MZ7LM") || Contains(mu, L"MZ7LH"))
    {
        return TRUE;
    }

    return FALSE;
}

} // namespace detail

namespace {

// ── attributeString[] — CrystalDiskInfo AtaSmart.h:116-169 ────────────
//
// Indexed by VENDOR_ID (CdiSmart.h:274-340), 51 entries.  The numeric
// values of VENDOR_ID are load-bearing: this table and kSsdVendorString
// are both indexed with them.

const wchar_t* const attributeString[SSD_VENDOR_STRING_COUNT] =
{
    L"Smart",
    L"SmartSsd",
    L"SmartMtron",
    L"SmartIndilinx",
    L"SmartJMicron",
    L"SmartIntel",
    L"SmartSamsung",
    L"SmartSandForce",
    L"SmartMicron",
    L"SmartOcz",
    L"SmartSeagate",
    L"SmartWdc",
    L"SmartPlextor",
    L"SmartSanDisk",
    L"SmartOczVector",
    L"SmartToshiba",
    L"SmartCorsair",
    L"SmartKingston",
    L"SmartMicronMU03",
    L"SmartNVMe",
    L"SmartRealtek",
    L"SmartSKhynix",
    L"SmartKioxia",
    L"SmartSsstc",
    L"SmartIntelDc",
    L"SmartApacer",
    L"SmartSiliconMotion",
    L"SmartPhison",
    L"SmartMarvell",
    L"SmartMaxiotek",
    L"SmartYmtc",
    L"SmartScy",
    L"SmartJMicron60x",
    L"SmartJMicron61x",
    L"SmartJMicron66x",
    L"SmartSeagateIronWolf",
    L"SmartSeagateBarraCuda",
    L"SmartSanDiskGb",
    L"SmartKingstonSuv",
    L"SmartKingstonKC600",
    L"SmartKingstonDC500",
    L"SmartKingstonSA400",
    L"SmartRecadata",
    L"SmartSanDiskDell",
    L"SmartSanDiskHp",
    L"SmartSanDiskHpVenus",
    L"SmartSanDiskLenovo",
    L"SmartSanDiskLenovoHelenVenus",
    L"SmartSanDiskCloud",
    L"SmartSiliconMotionCVC",
    L"SmartAdataIndustrial",
};
static_assert(_countof(attributeString) == SSD_VENDOR_STRING_COUNT,
              "attributeString[] must stay 51 entries wide");

// Upstream indexes ssdVendorString[asi.DiskVendorId] blindly, and
// SSD_VENDOR_MAX is 99 — a DiskVendorId above 50 would read past the end
// of the 51 entry table.  Classification only ever produces values that
// are known to be in range, but the port clamps rather than trusting the
// invariant (see the task note on kSsdVendorString).

std::wstring SsdVendorStringFor(DWORD diskVendorId)
{
    if (diskVendorId >= static_cast<DWORD>(SSD_VENDOR_STRING_COUNT))
    {
        return std::wstring();
    }
    return kSsdVendorString[diskVendorId];
}

// ═══════════════════════════════════════════════════════════════════════
//  IsSsdXxx() — file local statics.  Upstream signatures are
//  BOOL IsSsdXxx(ATA_SMART_INFO& asi); every one of them now also takes
//  the FlagLife*/NandWrites side channel context fetched once by
//  CheckSsdSupport().  Predicates that touch no flag simply ignore it.
//
//  Evaluation ORDER matters — IsSsdGeneral() must run last, because it
//  resets HostReadsWritesUnit to UNKNOWN, and CheckSsdSupport() is an
//  if/else-if chain: the first predicate to answer TRUE wins.
// ═══════════════════════════════════════════════════════════════════════

// AtaSmart.cpp:5017-5037
BOOL IsSsdOld(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    std::wstring model = asi.Model;
    model = ToUpperCopy(model);   // upstream: model.MakeUpper();

    return StartsWith(model, L"OCZ")
        || StartsWith(model, L"SPCC")
        || StartsWith(model, L"PATRIOT")
        || Contains(model, L"SOLID")
        || Contains(model, L"SSD")
        || Contains(model, L"SILICONHARDDISK")
        || StartsWith(model, L"PHOTOFAST")
        || StartsWith(model, L"STT_FTM")
        || StartsWith(model, L"SUPER TALENT")
        || StartsWith(model, L"SANDFORCE")
        || StartsWith(model, L"HANYE")
        || StartsWith(model, L"INTEL")
        || StartsWith(model, L"TOSHIBA THNS")
        || StartsWith(model, L"CSSD");   // upstream: // TOSHIBA CFD
}

// AtaSmart.cpp:5039-5042
BOOL IsSsdMtron(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    return ((asi.Attribute[0].Id == 0xBB && asi.AttributeCount == 1) || StartsWith(asi.Model, L"MTRON"));
}

// AtaSmart.cpp:5044-5062
BOOL IsSsdJMicron60x(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x0C
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0xC2
        && asi.Attribute[3].Id == 0xE5
        && asi.Attribute[4].Id == 0xE8
        && asi.Attribute[5].Id == 0xE9
//      && asi.Attribute[ 6].Id == 0xEA
//      && asi.Attribute[ 7].Id == 0xEB
        )
    {
        flagSmartType = TRUE;
    }

    // Upstream stored asi.IsRawValues8 here (CheckSsdSupport set
    // SmartKeyName alongside it).  IsRawValues8 has no DRIVE_INFO home and
    // is fully derivable from SmartKeyName, so the flag becomes the key.
    if (flagSmartType)
    {
        asi.SmartKeyName = L"SmartJMicron60x";
    }

    return flagSmartType;
}

// AtaSmart.cpp:5064-5090
BOOL IsSsdJMicron61x(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x02
        && asi.Attribute[2].Id == 0x03
        && asi.Attribute[3].Id == 0x05
        && asi.Attribute[4].Id == 0x07
        && asi.Attribute[5].Id == 0x08
        && asi.Attribute[6].Id == 0x09
        && asi.Attribute[7].Id == 0x0A
        && asi.Attribute[8].Id == 0x0C
        && asi.Attribute[9].Id == 0xA8
        && asi.Attribute[10].Id == 0xAF
        && asi.Attribute[11].Id == 0xC0
        && asi.Attribute[12].Id == 0xC2
//      && asi.Attribute[13].Id == 0xF0
//      && asi.Attribute[14].Id == 0xAA
//      && asi.Attribute[15].Id == 0xAD
        )
    {
        flagSmartType = TRUE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5092-5121
BOOL IsSsdJMicron66x(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x02
        && asi.Attribute[2].Id == 0x03
        && asi.Attribute[3].Id == 0x05
        && asi.Attribute[4].Id == 0x07
        && asi.Attribute[5].Id == 0x08
        && asi.Attribute[6].Id == 0x09
        && asi.Attribute[7].Id == 0x0A
        && asi.Attribute[8].Id == 0x0C
        && asi.Attribute[9].Id == 0xA7
        && asi.Attribute[10].Id == 0xA8
        && asi.Attribute[11].Id == 0xA9
        && asi.Attribute[12].Id == 0xAA
        && asi.Attribute[13].Id == 0xAD
        && asi.Attribute[14].Id == 0xAF
        )
    {
        flagSmartType = TRUE;
    }
    else if (StartsWith(asi.Model, L"ADATA SU700"))
    {
        flagSmartType = TRUE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5123-5163
BOOL IsSsdIndilinx(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xB8
        && asi.Attribute[4].Id == 0xC3
        && asi.Attribute[5].Id == 0xC4
//      && asi.Attribute[ 6].Id == 0xC5
//      && asi.Attribute[ 7].Id == 0xC6
//      && asi.Attribute[ 8].Id == 0xC7
//      && asi.Attribute[ 9].Id == 0xC8
//      && asi.Attribute[10].Id == 0xC9
//      && asi.Attribute[11].Id == 0xCA
//      && asi.Attribute[12].Id == 0xCB
//      && asi.Attribute[13].Id == 0xCC
//      && asi.Attribute[14].Id == 0xCD
//      && asi.Attribute[15].Id == 0xCE
//      && asi.Attribute[16].Id == 0xCF
//      && asi.Attribute[17].Id == 0xD0
//      && asi.Attribute[18].Id == 0xD1
//      && asi.Attribute[19].Id == 0xD2
//      && asi.Attribute[20].Id == 0xD3
        )
    {
        flagSmartType = TRUE;
    }

    /*
        asi.Model.Find(_T("OCZ-VERTEX")) == 0
    || asi.Model.Find(_T("G-Monster-V3")) == 0
    || asi.Model.Find(_T("G-Monster-V5")) == 0
    || (asi.Model.Find(_T("STT_FTM")) == 0 && asi.Model.Find(_T("GX")) > 0)
    || asi.Model.Find(_T("Solidata")) == 0
    */

    // Upstream stored asi.IsRawValues8 here; folded into SmartKeyName (see
    // the banner).
    if (flagSmartType)
    {
        asi.SmartKeyName = L"SmartIndilinx";
    }

    return flagSmartType;
}

// AtaSmart.cpp:5165-5169
BOOL IsSsdIntelDc(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    // https://github.com/hiyohiyo/CrystalDiskInfo/issues/18
    return (Contains(asi.Model, L"INTEL SSDSCKHB"));
}

// AtaSmart.cpp:5171-5199
BOOL IsSsdIntel(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (asi.Attribute[0].Id == 0x03
        && asi.Attribute[1].Id == 0x04
        && asi.Attribute[2].Id == 0x05
        && asi.Attribute[3].Id == 0x09
        && asi.Attribute[4].Id == 0x0C
        )
    {
        if (asi.Attribute[5].Id == 0xC0 && asi.Attribute[6].Id == 0xE8 && asi.Attribute[7].Id == 0xE9)
        {
            flagSmartType = TRUE;
        }
        else if (asi.Attribute[5].Id == 0xC0 && asi.Attribute[6].Id == 0xE1)
        {
            flagSmartType = TRUE;
        }
        else if (asi.Attribute[5].Id == 0xAA && asi.Attribute[6].Id == 0xAB && asi.Attribute[7].Id == 0xAC)
        {
            flagSmartType = TRUE;
        }
    }

    return (Contains(modelUpper, L"INTEL") || Contains(modelUpper, L"SOLIDIGM") || flagSmartType == TRUE);
}

// AtaSmart.cpp:5228-5296
//
// http://www.samsung.com/us/business/oem-solutions/pdfs/General_NSSD_25_SATA_III_Spec_0.2.pdf
BOOL IsSsdSamsung(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    // SM951
    if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xAA
        && asi.Attribute[4].Id == 0xAB
        && asi.Attribute[5].Id == 0xAC
        && asi.Attribute[6].Id == 0xAD
        && asi.Attribute[7].Id == 0xAE
        && asi.Attribute[8].Id == 0xB2
        && asi.Attribute[9].Id == 0xB4
        )
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }
    else if (asi.Attribute[0].Id == 0x09
        && asi.Attribute[1].Id == 0x0C
        && asi.Attribute[2].Id == 0xB2
        && asi.Attribute[3].Id == 0xB3
        && asi.Attribute[4].Id == 0xB4
        )
    {
        flagSmartType = TRUE;
    }
    else
    if (asi.Attribute[0].Id == 0x09
        && asi.Attribute[1].Id == 0x0C
        && asi.Attribute[2].Id == 0xB1
        && asi.Attribute[3].Id == 0xB2
        && asi.Attribute[4].Id == 0xB3
        && asi.Attribute[5].Id == 0xB4
        && asi.Attribute[6].Id == 0xB7
        )
    {
        flagSmartType = TRUE;
    }
    else
    if (asi.Attribute[0].Id == 0x09
        && asi.Attribute[1].Id == 0x0C
        && asi.Attribute[2].Id == 0xAF
        && asi.Attribute[3].Id == 0xB0
        && asi.Attribute[4].Id == 0xB1
        && asi.Attribute[5].Id == 0xB2
        && asi.Attribute[6].Id == 0xB3
        && asi.Attribute[7].Id == 0xB4
        )
    {
        flagSmartType = TRUE;
    }
    else
    if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xB1
        && asi.Attribute[4].Id == 0xB3
        && asi.Attribute[5].Id == 0xB5
        && asi.Attribute[6].Id == 0xB6
        )
    {
        flagSmartType = TRUE;
    }

    return ((Contains(asi.Model, L"SAMSUNG") && asi.IsSsd) || (Contains(asi.Model, L"MZ-") && asi.IsSsd) || flagSmartType == TRUE);
}

// AtaSmart.cpp:5298-5350
BOOL IsSsdSandForce(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0x0D
        && asi.Attribute[5].Id == 0x64
        && asi.Attribute[6].Id == 0xAA
        )
    {
        flagSmartType = TRUE;
    }

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xAB
        && asi.Attribute[5].Id == 0xAC
        )
    {
        flagSmartType = TRUE;
    }

    // TOSHIBA + SandForce
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1116;id=diskinfo#1116
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1136;id=diskinfo#1136
    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x02
        && asi.Attribute[2].Id == 0x03
        && asi.Attribute[3].Id == 0x05
        && asi.Attribute[4].Id == 0x07
        && asi.Attribute[5].Id == 0x08
        && asi.Attribute[6].Id == 0x09
        && asi.Attribute[7].Id == 0x0A
        && asi.Attribute[8].Id == 0x0C
        && asi.Attribute[9].Id == 0xA7
        && asi.Attribute[10].Id == 0xA8
        && asi.Attribute[11].Id == 0xA9
        && asi.Attribute[12].Id == 0xAA
        && asi.Attribute[13].Id == 0xAD
        && asi.Attribute[14].Id == 0xAF
        && asi.Attribute[15].Id == 0xB1
        )
    {
        flagSmartType = TRUE;
    }

    // The model-string arm answers TRUE too, so the key has to be written
    // for the whole result, not just for flagSmartType.  Upstream stored
    // asi.IsRawValues7 at the CheckSsdSupport() call site; folded into
    // SmartKeyName here (see the banner).
    const BOOL result = (Contains(asi.Model, L"SandForce") || flagSmartType == TRUE);
    if (result)
    {
        asi.SmartKeyName = L"SmartSandForce";
    }
    return result;
}

// Micron Crucial
// AtaSmart.cpp:5353-5385
BOOL IsSsdMicronMU03(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if ((
           StartsWith(modelUpper, L"MICRON_M600") || StartsWith(modelUpper, L"MICRON M600")
        || StartsWith(modelUpper, L"MICRON_M550") || StartsWith(modelUpper, L"MICRON M550")
        || StartsWith(modelUpper, L"MICRON_M510") || StartsWith(modelUpper, L"MICRON M510")
        || StartsWith(modelUpper, L"MICRON_M500") || StartsWith(modelUpper, L"MICRON M500")
        || StartsWith(modelUpper, L"MICRON_1300") || StartsWith(modelUpper, L"MICRON 1300")
        || StartsWith(modelUpper, L"MICRON_1100") || StartsWith(modelUpper, L"MICRON 1100") || StartsWith(modelUpper, L"MTFDDA")))
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
        flagSmartType = TRUE;
    }
    else if (
        (  Contains(modelUpper, L"M500SSD")
        || Contains(modelUpper, L"MX500SSD") || Contains(modelUpper, L"BX500SSD")
        || Contains(modelUpper, L"MX300SSD") || Contains(modelUpper, L"BX300SSD")
        || Contains(modelUpper, L"MX200SSD") || Contains(modelUpper, L"BX200SSD")
        || Contains(modelUpper, L"MX100SSD") || Contains(modelUpper, L"BX100SSD")
        || StartsWith(modelUpper, L"MTFD"))
        && !Contains(asi.FirmwareRev, L"MU01"))   // upstream: != -1
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
        flagSmartType = TRUE;
    }

    return flagSmartType;
}

// Micron RealSSD & Crucial
// AtaSmart.cpp:5388-5423
BOOL IsSsdMicron(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xAA
        && asi.Attribute[5].Id == 0xAB
        && asi.Attribute[6].Id == 0xAC
        && asi.Attribute[7].Id == 0xAD
        && asi.Attribute[8].Id == 0xAE
        && asi.Attribute[9].Id == 0xB5
        && asi.Attribute[10].Id == 0xB7
        )
    {
        flagSmartType = TRUE;
    }

    return StartsWith(modelUpper, L"P600") || StartsWith(modelUpper, L"C600")
        || StartsWith(modelUpper, L"M6-") || StartsWith(modelUpper, L"M600")
        || StartsWith(modelUpper, L"P500")
        || (StartsWith(modelUpper, L"C500") && !StartsWith(asi.FirmwareRev, L"H")) // workaround for Maxiotek C500
        || StartsWith(modelUpper, L"M5-") || StartsWith(modelUpper, L"M500")
        || StartsWith(modelUpper, L"P400") || StartsWith(modelUpper, L"C400")
        || StartsWith(modelUpper, L"M4-") || StartsWith(modelUpper, L"M400")
        || StartsWith(modelUpper, L"P300") || StartsWith(modelUpper, L"C300")
        || StartsWith(modelUpper, L"M3-") || StartsWith(modelUpper, L"M300")
        || (StartsWith(modelUpper, L"CT") && Contains(modelUpper, L"SSD"))   // upstream: != -1
        || StartsWith(modelUpper, L"CRUCIAL") || StartsWith(modelUpper, L"MICRON")
        || StartsWith(modelUpper, L"MTFD")
        || flagSmartType == TRUE;
}

// AtaSmart.cpp:5425-5454
BOOL IsSsdOcz(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    // OCZ-TRION100 2015/11/25
    if (StartsWith(modelUpper, L"OCZ-TRION"))
    {
        flagSmartType = TRUE;
    }
    // 2012/3/11
    // OCZ-PETROL - https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=553;id=diskinfo#553
    // OCZ-OCTANE S2 - https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=577;id=diskinfo#577
    // OCZ-VERTEX 4 - http://imageshack.us/a/img269/7506/ocz2.png
    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x03
        && asi.Attribute[2].Id == 0x04
        && asi.Attribute[3].Id == 0x05
        && asi.Attribute[4].Id == 0x09
        && asi.Attribute[5].Id == 0x0C
        && asi.Attribute[6].Id == 0xE8
        && asi.Attribute[7].Id == 0xE9
        )
    {
        flagSmartType = TRUE;
    }

    return (StartsWith(modelUpper, L"OCZ") && flagSmartType == TRUE);
}

// AtaSmart.cpp:5456-5522
BOOL IsSsdOczVector(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    // Radeon R7 2024/06/13
    if (StartsWith(modelUpper, L"RADEON R7"))
    {
        flagSmartType = TRUE;
        return TRUE;
    }

    /*
    // 2013/1/19
    // OCZ-VECTOR - https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1031;id=diskinfo#1031
    if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xAB
        && asi.Attribute[4].Id == 0xAE
        && asi.Attribute[5].Id == 0xBB
        && asi.Attribute[6].Id == 0xC3
        && asi.Attribute[7].Id == 0xC4
        )
    {
        flagSmartType = TRUE;
    }
    // 2013/3/24
    // OCZ-VECTOR - FW 2.0
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1185;id=diskinfo#1185
    if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xAB
        && asi.Attribute[4].Id == 0xAE
        && asi.Attribute[5].Id == 0xC3
        && asi.Attribute[6].Id == 0xC4
        )
    {
        flagSmartType = TRUE;
    }
    */

    // 2015/11/25
    // PANASONIC RP-SSB240GAK
    // https://crystalmark.info/board/c-board.cgi?cmd=one;no=500;id=#500
    if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xAB
        && asi.Attribute[4].Id == 0xAE
        && asi.Attribute[5].Id == 0xC3
        && asi.Attribute[6].Id == 0xC4
        && asi.Attribute[7].Id == 0xC5
        && asi.Attribute[8].Id == 0xC6
        )
    {
        flagSmartType = TRUE;
    }
    if (StartsWith(modelUpper, L"PANASONIC RP-SSB"))
    {
        flagSmartType = TRUE;
    }

    return (StartsWith(modelUpper, L"OCZ") || flagSmartType == TRUE);
}

// AtaSmart.cpp:5524-5534
BOOL IsSsdSsstc(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (Contains(asi.Model, L"CV8-") || Contains(asi.Model, L"CVB-") || Contains(asi.Model, L"ER2-"))
    {
        flagSmartType = TRUE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5536-5560
BOOL IsSsdPlextor(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    // 2012/10/10
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=739;id=diskinfo#739
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=829;id=diskinfo#829
    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xB1
        && asi.Attribute[5].Id == 0xB2
        && asi.Attribute[6].Id == 0xB5
        && asi.Attribute[7].Id == 0xB6
        )
    {
        flagSmartType = TRUE;
    }

    // Added CFD's SSD
    // Added LITEON CV6-CQ (2018/9/17)
    return  StartsWith(asi.Model, L"PLEXTOR") || StartsWith(asi.Model, L"LITEON") || StartsWith(asi.Model, L"CV6-CQ") || StartsWith(asi.Model, L"CSSD-S6T128NM3PQ") || StartsWith(asi.Model, L"CSSD-S6T256NM3PQ")
        || flagSmartType == TRUE;
}

// AtaSmart.cpp:5562-5699
//
// The only predicate that writes DiskVendorId itself: CheckSsdSupport()'s
// IsSsdSanDisk branch deliberately omits the assignment (it just reads
// DiskVendorId back to index the vendor string table).  Preserve that.
BOOL IsSsdSanDisk(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;

    // 2013/10/7
    // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1425;id=diskinfo#1425
    // 2020/07/25
    //
    if (Contains(asi.Model, L"SanDisk") || Contains(asi.Model, L"SD Ultra") || Contains(asi.Model, L"SDLF1"))
    {
        asi.DiskVendorId = SSD_VENDOR_SANDISK; // Default Vendor ID for SanDisk
        flagSmartType = TRUE;

        if (
              (Contains(asi.Model, L"X600") && Contains(asi.Model, L"2280")) // https://crystalmark.info/board/c-board.cgi?cmd=one;no=2123;id=#2123
            || Contains(asi.Model, L"X400")
            || Contains(asi.Model, L"X300")
            || Contains(asi.Model, L"X110")
            || Contains(asi.Model, L"SD5")
            )
        {
            if (asi.Attribute[2].Id == 0xAF || asi.Attribute[3].Id == 0xAF)
            {
                asi.SmartKeyName = L"SmartSanDiskDell";
            }
            else
            {
                asi.SmartKeyName = L"SmartSanDiskGb";
            }
            life.FlagLifeSanDisk1 = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;

        }
        else if (Contains(asi.Model, L"Z400"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
            asi.SmartKeyName = L"SmartSanDiskDell";
        }
        // 2022/04/24
        // https://osdn.net/projects/crystaldiskinfo/ticket/44354
        else if (ContainsAfter0(asi.Model, L"1006")) // HP OEM
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_16MB;
            if (ContainsAfter0(asi.Model, L"8U"))
            {
                asi.DiskVendorId = SSD_VENDOR_SANDISK_HP_VENUS;
                asi.SmartKeyName = L"SmartSanDiskHpVenus";
            }
            else
            {
                asi.DiskVendorId = SSD_VENDOR_SANDISK_HP;
                asi.SmartKeyName = L"SmartSanDiskHp";
            }
        }
        else if (Contains(asi.Model, L"G1001")) // Lenovo OEM
        {
            life.FlagLifeSanDiskLenovo = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
            if (ContainsAfter0(asi.Model, L"6S") || ContainsAfter0(asi.Model, L"7S") || ContainsAfter0(asi.Model, L"8U"))
            {
                asi.DiskVendorId = SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS;
                asi.SmartKeyName = L"SmartSanDiskLenovoHelenVenus";
            }
            // https://crystalmark.info/board/c-board.cgi?cmd=one;no=3176;id=#3176
            else if (Contains(asi.Model, L"SD9SB"))
            {
                asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
                life.NandWrites1MB = TRUE;         // upstream: NandWritesUnit = NAND_WRITES_1MB
                asi.SmartKeyName = L"SmartSanDiskGb";
            }
            else
            {
                asi.DiskVendorId = SSD_VENDOR_SANDISK_LENOVO;
                asi.SmartKeyName = L"SmartSanDiskLenovo";
            }
        }
        else if (Contains(asi.Model, L"G1012") || Contains(asi.Model, L"Z400s 2.5")) // DELL OEM
        {
            asi.DiskVendorId = SSD_VENDOR_SANDISK_DELL;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
            asi.SmartKeyName = L"SmartSanDiskDell";
        }
        else if (Contains(asi.Model, L"SSD P4"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
            life.FlagLifeSanDiskUsbMemory = TRUE; // No Life Report
            asi.SmartKeyName = L"SmartSanDisk";
        }
        else if (Contains(asi.Model, L"iSSD P4"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
            asi.SmartKeyName = L"SmartSanDiskGb";
        }
        else if (
           Contains(asi.Model, L"SDSSDP")
        || Contains(asi.Model, L"SDSSDRC")
        )
        {
            life.FlagLifeSanDisk0_1 = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
            asi.SmartKeyName = L"SmartSanDisk";
        }
        else if (
           Contains(asi.Model, L"SSD U100")
        || Contains(asi.Model, L"SSD U110")
        || Contains(asi.Model, L"SSD i100")
        || Contains(asi.Model, L"SSD i110")
        || Contains(asi.Model, L"pSSD")
            )
        {
            life.FlagLifeSanDiskUsbMemory = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
            asi.SmartKeyName = L"SmartSanDisk";
        }
        else if (
            // CloudSpeed ECO Gen II Eco SSD
            Contains(asi.Model, L"SDLF1CRR-")
            || Contains(asi.Model, L"SDLF1DAR-")
            // CloudSpeed ECO Gen II Ultra SSD
            || Contains(asi.Model, L"SDLF1CRM-")
            || Contains(asi.Model, L"SDLF1DAM-")
            )
        {
            asi.DiskVendorId = SSD_VENDOR_SANDISK_CLOUD;
            life.FlagLifeSanDiskCloud = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
            asi.SmartKeyName = L"SmartSanDiskCloud";
        }
        else
        {
            life.FlagLifeSanDisk1 = TRUE;
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
            asi.SmartKeyName = L"SmartSanDiskGb";
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:5701-5755
//
// Writes SmartKeyName itself; CheckSsdSupport()'s branch only adds
// DiskVendorId and SsdVendorString.  Preserve that asymmetry.
BOOL IsSsdKingston(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;

    if (Contains(asi.Model, L"KINGSTON"))
    {
        if (Contains(asi.Model, L"SM2280") || Contains(asi.Model, L"SEDC400") || Contains(asi.Model, L"SKC310") || Contains(asi.Model, L"SHSS") || Contains(asi.Model, L"SUV300") || Contains(asi.Model, L"SKC400"))
        {
            flagSmartType = TRUE;
            asi.SmartKeyName = L"SmartKingston";
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
        else if (Contains(asi.Model, L"SA400"))
        {
            flagSmartType = TRUE;
            // https://github.com/hiyohiyo/CrystalDiskInfo/issues/162
            if (StartsWith(asi.FirmwareRev, L"03070009"))
            {
                life.FlagLifeRawValue = FALSE;
            }
            // https://github.com/hiyohiyo/CrystalDiskInfo/issues/201
            else if (StartsWith(asi.FirmwareRev, L"SBFK62C3"))
            {
            //  life.FlagLifeNoReport = TRUE;
                life.FlagLifeRawValue = TRUE;
            }
            else
            {
                life.FlagLifeRawValue = TRUE;
            }
            asi.SmartKeyName = L"SmartKingstonSA400";
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
        else if (Contains(asi.Model, L"KC600"))
        {
            flagSmartType = TRUE;
            asi.SmartKeyName = L"SmartKingstonKC600";
            asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
        }
        else if (Contains(asi.Model, L"DC500"))
        {
            flagSmartType = TRUE;
            asi.SmartKeyName = L"SmartKingstonDC500";
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
        else if (Contains(asi.Model, L"SUV400") || Contains(asi.Model, L"SUV500"))
        {
            flagSmartType = TRUE;
            asi.SmartKeyName = L"SmartKingstonSuv";
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:5757-5771
BOOL IsSsdCorsair(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (StartsWith(asi.Model, L"Corsair"))
    {
        flagSmartType = TRUE;
        if (Contains(asi.Model, L"Voyager GTX"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_1MB;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:5773-5800
BOOL IsSsdToshiba(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (Contains(modelUpper, L"TOSHIBA") && asi.IsSsd)
    {
        flagSmartType = TRUE;
        if (Contains(asi.Model, L"THNSNC") || Contains(asi.Model, L"THNSNJ") || Contains(asi.Model, L"THNSNK")
        ||  Contains(asi.Model, L"KSG60")
        ||  Contains(asi.Model, L"TL100") || Contains(asi.Model, L"TR150") || Contains(asi.Model, L"TR200")
        )
        {
            // TOSHIBA HG3
            // https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1106;id=diskinfo#1106
            // TOSHIBA KSG60ZMV
            // https://crystalmark.info/board/c-board.cgi?cmd=one;no=2425;id=#2425
            asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
        }
        else
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:5802-5824
//
// Also writes HostReadsWritesUnit and SmartKeyName; CheckSsdSupport()'s
// branch repeats both (upstream does the same).
BOOL IsSsdRealtek(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xA1
        && asi.Attribute[5].Id == 0xA2
        && asi.Attribute[6].Id == 0xA3
        && asi.Attribute[7].Id == 0xA4
        && asi.Attribute[8].Id == 0xA6
        && asi.Attribute[9].Id == 0xA7
        )
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        asi.SmartKeyName = L"SmartRealtek";
    }

    return flagSmartType;
}

// AtaSmart.cpp:5826-5860
//
// Note the trailing else: HostReadsWritesUnit is forced to GB for *every*
// drive that reaches here, including non-SK hynix models.  That is
// harmless because the result is only consulted when this predicate
// answers TRUE, but it is load-bearing for those drives, so keep it.
BOOL IsSsdSKhynix(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;
    if (Contains(asi.Model, L"SK hynix") || StartsWith(asi.Model, L"HFS") || StartsWith(asi.Model, L"SHG"))
    {
        flagSmartType = TRUE;
        asi.SmartKeyName = L"SmartSKhynix";
    }

    // https://crystalmark.info/board/c-board.cgi?cmd=one;no=1772;id=#1772
    if (
       (Contains(asi.Model, L"HFS") && Contains(asi.Model, L"TND")) // SL300
    || (Contains(asi.Model, L"HFS") && Contains(asi.Model, L"MND")) // SC210
    )
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        life.FlagLifeRawValueIncrement = TRUE;
    }
    else if (Contains(asi.Model, L"HFS") && Contains(asi.Model, L"TNF"))
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        life.FlagLifeRawValue = TRUE;
    }
    else if (Contains(asi.Model, L"SC311") || Contains(asi.Model, L"SC401"))
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
        life.FlagLifeRawValue = TRUE;
    }
    else
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5862-5872
BOOL IsSsdKioxia(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    if (Contains(asi.Model, L"KIOXIA"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
        asi.SmartKeyName = L"SmartKioxia";
    }
    return flagSmartType;
}

// AtaSmart.cpp:5874-5890
//
// Upstream quirk (preserved): the three firmware prefix tests are *not*
// nested inside the model test — they are further arms of the same ||
// chain, so any drive whose firmware revision starts with "AP", "SF" or
// "PN" reports as an Apacer, whatever its model says.
BOOL IsSsdApacer(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;
    if (StartsWith(asi.Model, L"Apacer")
    ||  StartsWith(asi.Model, L"ZADAK")
    ||  StartsWith(asi.FirmwareRev, L"AP")
    ||  StartsWith(asi.FirmwareRev, L"SF")
    ||  StartsWith(asi.FirmwareRev, L"PN")
    )
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
        life.FlagLifeRawValue = TRUE;
        asi.SmartKeyName = L"SmartApacer";
    }
    return flagSmartType;
}

// AtaSmart.cpp:5892-5902
BOOL IsSsdYmtc(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    if (Contains(asi.Model, L"ZHITAI"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
        asi.SmartKeyName = L"SmartYmtc";
    }
    return flagSmartType;
}

// AtaSmart.cpp:5904-5914
BOOL IsSsdScy(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    if (StartsWith(asi.Model, L"SCY")) // SCY S500
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
        asi.SmartKeyName = L"SmartScy";
    }
    return flagSmartType;
}

// AtaSmart.cpp:5916-5926
BOOL IsSsdRecadata(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    if (StartsWith(asi.Model, L"RECADATA"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5928-5938
BOOL IsSsdSiliconMotionCVC(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;
    if (Contains(asi.Model, L"CVC-"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }

    return flagSmartType;
}

// AtaSmart.cpp:5940-6083
BOOL IsSsdSiliconMotion(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;

    if (   asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xA0
        && asi.Attribute[5].Id == 0xA1
        && asi.Attribute[6].Id == 0xA3
        && asi.Attribute[7].Id == 0xA4
        && asi.Attribute[8].Id == 0xA5
        && asi.Attribute[9].Id == 0xA6
        && asi.Attribute[10].Id == 0xA7
        && asi.Attribute[11].Id == 0xA8
        && asi.Attribute[12].Id == 0xA9
        && asi.Attribute[13].Id == 0xAF
        && asi.Attribute[14].Id == 0xB0
        && asi.Attribute[15].Id == 0xB1
        && asi.Attribute[16].Id == 0xB2
        && asi.Attribute[17].Id == 0xB5
        && asi.Attribute[18].Id == 0xB6
        && asi.Attribute[19].Id == 0xC0
        )
    {
        flagSmartType = TRUE;
    }
    else if ( // ADATA SX950 https://crystalmark.info/board/c-board.cgi?cmd=one;no=1819;id=
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xA0
        && asi.Attribute[5].Id == 0xA1
        && asi.Attribute[6].Id == 0xA3
        && asi.Attribute[7].Id == 0xA4
        && asi.Attribute[8].Id == 0xA5
        && asi.Attribute[9].Id == 0xA6
        && asi.Attribute[10].Id == 0xA7
        && asi.Attribute[11].Id == 0x94
        && asi.Attribute[12].Id == 0x95
        && asi.Attribute[13].Id == 0x96
        && asi.Attribute[14].Id == 0x97
        && asi.Attribute[15].Id == 0xA9
        && asi.Attribute[16].Id == 0xB1
        && asi.Attribute[17].Id == 0xB5
        && asi.Attribute[18].Id == 0xB6
        && asi.Attribute[19].Id == 0xBB
        )
    {
        flagSmartType = TRUE;
    }
    else if (
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0x94
        && asi.Attribute[5].Id == 0x95
        && asi.Attribute[6].Id == 0x96
        && asi.Attribute[7].Id == 0x97
        && asi.Attribute[8].Id == 0x9F
        && asi.Attribute[9].Id == 0xA0
        && asi.Attribute[10].Id == 0xA1
        )
    {
        flagSmartType = TRUE;
    }
    else if (
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xA0
        && asi.Attribute[5].Id == 0xA1
        && asi.Attribute[6].Id == 0xA3
        && asi.Attribute[7].Id == 0xA4
        && asi.Attribute[8].Id == 0xA5
        && asi.Attribute[9].Id == 0xA6
        && asi.Attribute[10].Id == 0xA7
        )
    {
        flagSmartType = TRUE;
    }
    else if (
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0xA0
        && asi.Attribute[5].Id == 0xA1
        && asi.Attribute[6].Id == 0xA3
        && asi.Attribute[7].Id == 0x94
        && asi.Attribute[8].Id == 0x95
        && asi.Attribute[9].Id == 0x96
        && asi.Attribute[10].Id == 0x97
        )
    {
        flagSmartType = TRUE;
    }

    // Transcend
    else if (StartsWith(asi.Model, L"TS"))
    {
        if ((asi.SmartReadData[400] == 'T' && asi.SmartReadData[401] == 'S') // Transcend
        ||   asi.SmartReadData[400] == 'S' && asi.SmartReadData[401] == 'M') // Silicon Motion
        {
            flagSmartType = TRUE;
        }
    }
    else if (StartsWith(asi.Model, L"ADATA SX950"))
    {
        flagSmartType = TRUE;
    }

    if (flagSmartType)
    {
        if (StartsWith(asi.Model, L"SSD") && StartsWith(asi.FirmwareRev, L"FW")) // for Goldenfir SSD
        {
        // Disabled for harmful to other products (v8.12.8)
        //  life.FlagLifeRawValueIncrement = TRUE;
        }
        // WINTEC
        else if (StartsWith(asi.Model, L"WT200") || StartsWith(asi.Model, L"WT100") || StartsWith(asi.Model, L"WT "))
        {

        }
        else if (StartsWith(asi.Model, L"tecmiyo")) // https://github.com/hiyohiyo/CrystalDiskInfo/issues/191
        {

        }
        else if (StartsWith(asi.Model, L"ADATA SU650") && StartsWith(asi.FirmwareRev, L"XD0R3C0A")) // Twitter DM
        {

        }
        else
        {
            life.FlagLifeRawValue = TRUE;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:6085-6142
BOOL IsSsdPhison(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;

    if (   asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA8
        && asi.Attribute[4].Id == 0xAA
        && asi.Attribute[5].Id == 0xAD
        && asi.Attribute[6].Id == 0xC0
        && asi.Attribute[7].Id == 0xC2 // with Temperature Sensor
        && asi.Attribute[8].Id == 0xDA
        && asi.Attribute[9].Id == 0xE7
        && asi.Attribute[10].Id == 0xF1
    )
    {
        flagSmartType = TRUE;
    }
    else if(
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA8
        && asi.Attribute[4].Id == 0xAA
        && asi.Attribute[5].Id == 0xAD
        && asi.Attribute[6].Id == 0xC0
        && asi.Attribute[7].Id == 0xDA
        && asi.Attribute[8].Id == 0xE7
        && asi.Attribute[9].Id == 0xF1
        )
    {
        flagSmartType = TRUE;
    }

    if (flagSmartType)
    {
        if (StartsWith(asi.Model, L"aigo PSSD P6"))
        {
            life.FlagLifeRawValue = FALSE;
        }
        else
        {
            life.FlagLifeRawValue = TRUE;
        }

        if (StartsWith(asi.FirmwareRev, L"S9"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_1MB;
        }
        else
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:6144-6163
BOOL IsSsdWdc(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (StartsWith(asi.Model, L"WDC ") || StartsWith(asi.Model, L"WD "))
    {
        flagSmartType = TRUE;

        if (Contains(asi.Model, L"SA530"))
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_16MB;
        }
        else
        {
            asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        }
    }

    return flagSmartType;
}

// AtaSmart.cpp:6165-6245
//
// All three arms write DiskVendorId = SSD_VENDOR_SEAGATE themselves;
// CheckSsdSupport()'s branch only reads it back.  Preserve that.
BOOL IsSsdSeagate(DRIVE_INFO& asi, SsdLifeCtx& life)
{
    BOOL flagSmartType = FALSE;

    if (   asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x05
        && asi.Attribute[2].Id == 0x09
        && asi.Attribute[3].Id == 0x0C
        && asi.Attribute[4].Id == 0x64
        && asi.Attribute[5].Id == 0x66
        && asi.Attribute[6].Id == 0x67
        && asi.Attribute[7].Id == 0xAA
        && asi.Attribute[8].Id == 0xAB
        && asi.Attribute[9].Id == 0xAC
        && asi.Attribute[10].Id == 0xAD
        && asi.Attribute[11].Id == 0xAE
        && asi.Attribute[12].Id == 0xB1
        && asi.Attribute[13].Id == 0xB7
        && asi.Attribute[14].Id == 0xBB
        )
    {
        flagSmartType = TRUE;
        asi.SmartKeyName = L"SmartSeagateIronWolf";
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        asi.DiskVendorId = SSD_VENDOR_SEAGATE;
    }
    else if (
           asi.Attribute[0].Id == 0x01
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0x10
        && asi.Attribute[4].Id == 0x11
        && asi.Attribute[5].Id == 0xA8
        && asi.Attribute[6].Id == 0xAA
        && asi.Attribute[7].Id == 0xAD
        && asi.Attribute[8].Id == 0xAE
        && asi.Attribute[9].Id == 0xB1
        && asi.Attribute[10].Id == 0xC0
        && asi.Attribute[11].Id == 0xC2
        && asi.Attribute[12].Id == 0xDA
        && asi.Attribute[13].Id == 0xE7
        && asi.Attribute[14].Id == 0xE8
        && asi.Attribute[15].Id == 0xE9
        && asi.Attribute[16].Id == 0xEB
        && asi.Attribute[17].Id == 0xF1
        && asi.Attribute[18].Id == 0xF2
        )
    {
        flagSmartType = TRUE;
        asi.SmartKeyName = L"SmartSeagate";
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        life.FlagLifeRawValue = TRUE;
        asi.DiskVendorId = SSD_VENDOR_SEAGATE;
    }
    else if (StartsWith(asi.Model, L"Seagate")
        || (!StartsWith(asi.Model, L"STT") && StartsWith(asi.Model, L"ST"))   // upstream: Find(L"STT") != 0 && Find(L"ST") == 0
        || (StartsWith(asi.Model, L"ZA"))
        )
    {
        flagSmartType = TRUE;
        if (Contains(asi.Model, L"BarraCuda"))
        {
            asi.SmartKeyName = L"SmartSeagateBarraCuda";
            life.FlagLifeRawValue = TRUE;
        }
        else if (Contains(asi.Model, L"HM") || Contains(asi.Model, L"FP"))
        {
            asi.SmartKeyName = L"SmartSeagate";
            life.FlagLifeRawValue = FALSE;
        }
        else
        {
            asi.SmartKeyName = L"SmartSeagate";
            life.FlagLifeRawValue = TRUE;
        }
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
        asi.DiskVendorId = SSD_VENDOR_SEAGATE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:6247-6296
//
// Note the early `return FALSE` for HANYE-Q55: it overrides an already
// TRUE flagSmartType, so a Marvell-pattern drive named HANYE-Q55 is
// deliberately rejected.
BOOL IsSsdMarvell(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    if (   asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA1
        && asi.Attribute[4].Id == 0xA4
        && asi.Attribute[5].Id == 0xA5
        && asi.Attribute[6].Id == 0xA6
        && asi.Attribute[7].Id == 0xA7
        )
    {
        flagSmartType = TRUE;
    }
    else if (asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA4
        && asi.Attribute[4].Id == 0xA5
        && asi.Attribute[5].Id == 0xA6
        && asi.Attribute[6].Id == 0xA7
        )
    {
        flagSmartType = TRUE;
    }

    if (flagSmartType)
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }

    // https://crystalmark.info/board/c-board.cgi?cmd=one;no=2476;id=#2476
    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (StartsWith(modelUpper, L"LEXAR") && ( StartsWith(asi.FirmwareRev, L"SN0") || StartsWith(asi.FirmwareRev, L"V6") ))
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
    }

    // https://crystalmark.info/board/c-board.cgi?cmd=one;no=2523;id=#2523
    if ((StartsWith(modelUpper, L"HANYE-Q55")))
    {
        return FALSE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:6298-6341
BOOL IsSsdMaxiotek(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (StartsWith(modelUpper, L"MAXIO"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }
    else if (StartsWith(modelUpper, L"CUSO C5S-EVO"))
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
    }
    // https://crystalmark.info/board/c-board.cgi?cmd=one;no=2523;id=#2523
    else if (StartsWith(modelUpper, L"HANYE-Q55")
        && asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA4
        && asi.Attribute[4].Id == 0xA5
        && asi.Attribute[5].Id == 0xA6
        && asi.Attribute[6].Id == 0xA7
        )
    {
        flagSmartType = TRUE;
    }
    else if (
           asi.Attribute[0].Id == 0x05
        && asi.Attribute[1].Id == 0x09
        && asi.Attribute[2].Id == 0x0C
        && asi.Attribute[3].Id == 0xA7
        && asi.Attribute[4].Id == 0xA8
        && asi.Attribute[5].Id == 0xA9
        )
    {
        flagSmartType = TRUE;
    }

    return flagSmartType;
}

// AtaSmart.cpp:6343-6363
BOOL IsSsdAdataIndustrial(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    BOOL flagSmartType = FALSE;

    std::wstring modelUpper = ToUpperCopy(asi.Model);   // upstream: MakeUpper()

    if (StartsWith(modelUpper, L"ADATA_IM2S")
    ||  StartsWith(modelUpper, L"ADATA_IMSS")
    ||  StartsWith(modelUpper, L"ADATA_ISSS")
    ||  StartsWith(modelUpper, L"IM2S")
    ||  StartsWith(modelUpper, L"IMSS")
    ||  StartsWith(modelUpper, L"ISSS")
    )
    {
        flagSmartType = TRUE;
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
    }

    return flagSmartType;
}

// AtaSmart.cpp:6366-6390
//
// MUST be the last predicate: it resets HostReadsWritesUnit, throwing away
// whatever the drive actually reported, and then answers with IsSsd.
BOOL IsSsdGeneral(DRIVE_INFO& asi, SsdLifeCtx& /*life*/)
{
    asi.HostReadsWritesUnit = HOST_READS_WRITES_UNKNOWN;

    if (
        StartsWith(asi.Model, L"ADATA SP580")
    )
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_512B;
    }
    else if (
       StartsWith(asi.Model, L"LITEON IT LMT")
    )
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_32MB;
    }
    else if (
       StartsWith(asi.Model, L"LITEON S960")
    )
    {
        asi.HostReadsWritesUnit = HOST_READS_WRITES_GB;
    }

    return asi.IsSsd;
}

} // namespace

// ═══════════════════════════════════════════════════════════════════════
//  CAtaSmart::CheckSsdSupport — AtaSmart.cpp:4149-4982
//
//  Decides what a drive *is* (HDD / SSD vendor) and what its raw SMART
//  values *mean* (HostReadsWritesUnit, Life, HostWrites, HostReads,
//  NandWrites, GBytesErased, WearLevelingCount).  Both halves run here:
//  the classification chain, then the "Update Life" loop over the
//  attribute list.
//
//  This is the authoritative classifier — cdi::SetSmartKeyName() in
//  CdiAttributeName.cpp is only a fallback for drives that never reach
//  this function.  See the banner at the top of this file.
// ═══════════════════════════════════════════════════════════════════════

void CAtaSmart::CheckSsdSupport(DRIVE_INFO& asi)
{
    // One get-or-create fetch for the whole classification; the helpers
    // below are the only writers of the FlagLife*/NandWrites1MB channel.
    SsdLifeCtx& life = LifeCtx(asi);

    // ── Old SSD Detection ─────────────────────────────────────────────
    // May promote a drive to SSD before the HDD/SSD split below.
    if (IsSsdOld(asi, life))
    {
        asi.IsSsd = TRUE;
    }

    if (!asi.IsSsd) // HDD
    {
        asi.SmartKeyName = L"Smart";
        asi.DiskVendorId = HDD_GENERAL;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdAdataIndustrial(asi, life))
    {
        asi.SmartKeyName = L"SmartAdataIndustrial";
        asi.DiskVendorId = SSD_VENDOR_ADATA_INDUSTRIAL;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSanDisk(asi, life))
    {
        // IsSsdSanDisk already wrote DiskVendorId (and SmartKeyName).
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdWdc(asi, life))
    {
        asi.SmartKeyName = L"SmartWdc";
        asi.DiskVendorId = SSD_VENDOR_WDC;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSeagate(asi, life))
    {
        // IsSsdSeagate already wrote DiskVendorId (and SmartKeyName).
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdMtron(asi, life))
    {
        asi.SmartKeyName = L"SmartMtron";
        asi.DiskVendorId = SSD_VENDOR_MTRON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdToshiba(asi, life))
    {
        asi.SmartKeyName = L"SmartToshiba";
        asi.DiskVendorId = SSD_VENDOR_TOSHIBA;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdJMicron66x(asi, life))
    {
        asi.SmartKeyName = L"SmartJMicron66x";
        asi.DiskVendorId = SSD_VENDOR_JMICRON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdJMicron61x(asi, life))
    {
        asi.SmartKeyName = L"SmartJMicron61x";
        asi.DiskVendorId = SSD_VENDOR_JMICRON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdJMicron60x(asi, life))
    {
        // SmartKeyName set by the helper (upstream: IsRawValues8 = TRUE).
        asi.DiskVendorId = SSD_VENDOR_JMICRON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdIndilinx(asi, life))
    {
        // SmartKeyName set by the helper (upstream: IsRawValues8 = TRUE).
        asi.DiskVendorId = SSD_VENDOR_INDILINX;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdIntelDc(asi, life))
    {
        asi.SmartKeyName = L"SmartIntelDc";
        asi.DiskVendorId = SSD_VENDOR_INTEL_DC;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdIntel(asi, life))
    {
        asi.SmartKeyName = L"SmartIntel";
        asi.DiskVendorId = SSD_VENDOR_INTEL;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSamsung(asi, life))
    {
        asi.SmartKeyName = L"SmartSamsung";
        asi.DiskVendorId = SSD_VENDOR_SAMSUNG;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdMicronMU03(asi, life))
    {
        asi.SmartKeyName = L"SmartMicronMU03";
        asi.DiskVendorId = SSD_VENDOR_MICRON_MU03;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdMicron(asi, life))
    {
        asi.SmartKeyName = L"SmartMicron";
        asi.DiskVendorId = SSD_VENDOR_MICRON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSandForce(asi, life))
    {
        // SmartKeyName set by the helper (upstream: IsRawValues7 = TRUE).
        asi.DiskVendorId = SSD_VENDOR_SANDFORCE;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdOcz(asi, life))
    {
        asi.SmartKeyName = L"SmartOcz";
        asi.DiskVendorId = SSD_VENDOR_OCZ;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdOczVector(asi, life))
    {
        asi.SmartKeyName = L"SmartOczVector";
        asi.DiskVendorId = SSD_VENDOR_OCZ_VECTOR;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSsstc(asi, life))
    {
        asi.SmartKeyName = L"SmartSsstc";
        asi.DiskVendorId = SSD_VENDOR_SSSTC;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdPlextor(asi, life))
    {
        asi.SmartKeyName = L"SmartPlextor";
        asi.DiskVendorId = SSD_VENDOR_PLEXTOR;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdKingston(asi, life))
    {
        // SmartKeyName set by the helper.
        asi.DiskVendorId = SSD_VENDOR_KINGSTON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdCorsair(asi, life))
    {
        asi.SmartKeyName = L"SmartCorsair";
        asi.DiskVendorId = SSD_VENDOR_CORSAIR;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdRealtek(asi, life))
    {
        asi.SmartKeyName = L"SmartRealtek";
        asi.DiskVendorId = SSD_VENDOR_REALTEK;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSKhynix(asi, life))
    {
        asi.SmartKeyName = L"SmartSKhynix";
        asi.DiskVendorId = SSD_VENDOR_SKHYNIX;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdKioxia(asi, life))
    {
        asi.SmartKeyName = L"SmartKioxia";
        asi.DiskVendorId = SSD_VENDOR_KIOXIA;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSiliconMotionCVC(asi, life))
    {
        asi.SmartKeyName = L"SmartSiliconMotionCVC";
        asi.DiskVendorId = SSD_VENDOR_SILICONMOTION_CVC;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdSiliconMotion(asi, life))
    {
        asi.SmartKeyName = L"SmartSiliconMotion";
        asi.DiskVendorId = SSD_VENDOR_SILICONMOTION;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdPhison(asi, life))
    {
        asi.SmartKeyName = L"SmartPhison";
        asi.DiskVendorId = SSD_VENDOR_PHISON;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdMarvell(asi, life))
    {
        asi.SmartKeyName = L"SmartMarvell";
        asi.DiskVendorId = SSD_VENDOR_MARVELL;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdMaxiotek(asi, life))
    {
        asi.SmartKeyName = L"SmartMaxiotek";
        asi.DiskVendorId = SSD_VENDOR_MAXIOTEK;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdApacer(asi, life))
    {
        asi.SmartKeyName = L"SmartApacer";
        asi.DiskVendorId = SSD_VENDOR_APACER;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdYmtc(asi, life))
    {
        asi.SmartKeyName = L"SmartYmtc";
        asi.DiskVendorId = SSD_VENDOR_YMTC;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdScy(asi, life))
    {
        asi.SmartKeyName = L"SmartScy";
        asi.DiskVendorId = SSD_VENDOR_SCY;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdRecadata(asi, life))
    {
        asi.SmartKeyName = L"SmartRecadata";
        asi.DiskVendorId = SSD_VENDOR_RECADATA;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
    }
    else if (IsSsdGeneral(asi, life))
    {
        asi.DiskVendorId = SSD_GENERAL;
        asi.SsdVendorString = SsdVendorStringFor(asi.DiskVendorId);
        // attributeString[SSD_GENERAL] == L"SmartSsd"
        asi.SmartKeyName = attributeString[asi.DiskVendorId];
        return;
    }
    else
    {
        asi.DiskVendorId = HDD_GENERAL;
        // attributeString[HDD_GENERAL] == L"Smart"
        asi.SmartKeyName = attributeString[asi.DiskVendorId];
        return;
    }

// Update Life
    for (DWORD j = 0; j < asi.AttributeCount; j++)
    {
        switch (asi.Attribute[j].Id)
        {
        case 0xBB:
            if (asi.DiskVendorId == SSD_VENDOR_MTRON)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xCA:
            if (asi.DiskVendorId == SSD_VENDOR_MICRON || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03 || asi.DiskVendorId == SSD_VENDOR_INTEL_DC || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xD1:
            if (asi.DiskVendorId == SSD_VENDOR_INDILINX)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xC9:
            if (asi.DiskVendorId == SSD_VENDOR_SANDISK_HP || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS)
            {
                int lifeValue = -1;
                lifeValue = asi.Attribute[j].CurrentValue;
                if (lifeValue <= 0) { lifeValue = -1; }

                asi.Life = lifeValue;
            }
            break;
        case 0xE6:
            if (asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SANDISK)
            {
                int lifeValue = -1;

                if (life.FlagLifeSanDiskUsbMemory)
                {
                    lifeValue = -1;
                }
                else if (life.FlagLifeSanDisk0_1)
                {
                    lifeValue = 100 - (asi.Attribute[j].RawValue[1] * 256 + asi.Attribute[j].RawValue[0]) / 100;
                }
                else if (life.FlagLifeSanDisk1)
                {
                    lifeValue = 100 - asi.Attribute[j].RawValue[1];
                }
                else if (life.FlagLifeSanDiskLenovo)
                {
                    lifeValue = asi.Attribute[j].CurrentValue;
                }
                else
                {
                    lifeValue = 100 - asi.Attribute[j].RawValue[1];
                }

                if (lifeValue <= 0) { lifeValue = -1; }

                asi.Life = lifeValue;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi.DiskVendorId == SSD_VENDOR_SANDISK_DELL)
            {
                int lifeValue = -1;
                lifeValue = asi.Attribute[j].CurrentValue;
                if (lifeValue <= 0) { lifeValue = -1; }

                asi.Life = lifeValue;
            }
            break;
        case 0xE8:
            if (asi.DiskVendorId == SSD_VENDOR_PLEXTOR)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_OCZ)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            break;
        case 0xE9:
            if (asi.DiskVendorId == SSD_VENDOR_INTEL || asi.DiskVendorId == SSD_VENDOR_OCZ || asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi.DiskVendorId == SSD_VENDOR_SKHYNIX)
            {
                if (life.FlagLifeRawValue)
                {
                    asi.Life = asi.Attribute[j].RawValue[0];
                }
                else
                {
                    asi.Life = asi.Attribute[j].CurrentValue;
                }
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG && IsSamsungEnterpriseModel(asi.Model))
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life < 0 || asi.Life > 100) { asi.Life = -1; }
            }
            else if ((asi.DiskVendorId == SSD_VENDOR_SANDISK || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi.DiskVendorId == SSD_VENDOR_SANDISK_CLOUD) && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                if (life.NandWrites1MB)   // upstream: NandWritesUnit == NAND_WRITES_1MB
                {
                    asi.NandWrites = static_cast<INT>(B8toB32le(asi.Attribute[j].RawValue) / 1024);
                }
                else
                {
                    asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_PLEXTOR || asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SSSTC || asi.DiskVendorId == SSD_VENDOR_SEAGATE || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC)
            {
                asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_JMICRON || asi.DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_MAXIOTEK)
            {
                if (asi.HostReadsWritesUnit == HOST_READS_WRITES_512B)
                {
                    asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 2 / 1024 / 1024);
                }
                else
                {
                    asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
            }
            break;
        case 0xE1:
            if (asi.DiskVendorId == SSD_VENDOR_INTEL)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            break;
        case 0xEA:
            if (asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_SEAGATE
                || (asi.DiskVendorId == SSD_VENDOR_SKHYNIX && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
                )
            {
                asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            break;
        case 0xEB:
            if (asi.DiskVendorId == SSD_VENDOR_INTEL_DC)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            break;
        case 0xF1:
            if (asi.DiskVendorId == SSD_GENERAL)
            {
                if (asi.HostReadsWritesUnit == HOST_READS_WRITES_512B)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 2 / 1024 / 1024);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_1MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 1024);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 64);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
                else
                {
                    // upstream: empty
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_TOSHIBA && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_INTEL_DC)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_INTEL || asi.DiskVendorId == SSD_VENDOR_TOSHIBA || asi.DiskVendorId == SSD_VENDOR_KIOXIA || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDFORCE || asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi.DiskVendorId == SSD_VENDOR_CORSAIR || asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_REALTEK
                || asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SSSTC || asi.DiskVendorId == SSD_VENDOR_SKHYNIX || asi.DiskVendorId == SSD_VENDOR_PHISON || asi.DiskVendorId == SSD_VENDOR_SEAGATE || asi.DiskVendorId == SSD_VENDOR_MARVELL
                || asi.DiskVendorId == SSD_VENDOR_MAXIOTEK || asi.DiskVendorId == SSD_VENDOR_YMTC || asi.DiskVendorId == SSD_VENDOR_SCY || asi.DiskVendorId == SSD_VENDOR_RECADATA || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03
                || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS || asi.DiskVendorId == SSD_VENDOR_SANDISK_DELL || asi.DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL
                )
            {
                if (asi.HostReadsWritesUnit == HOST_READS_WRITES_512B)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 2 / 1024 / 1024);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_1MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 1024);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 64);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                {
                    asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                else
                {
                    asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG || asi.DiskVendorId == SSD_VENDOR_APACER || asi.DiskVendorId == SSD_VENDOR_JMICRON)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_PLEXTOR)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            break;
        case 0xF2:
            if (asi.DiskVendorId == SSD_GENERAL)
            {
                if (asi.HostReadsWritesUnit == HOST_READS_WRITES_512B)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 2 / 1024 / 1024);
                }
                // NB: upstream has no HOST_READS_WRITES_1MB arm here (0xF1
                // has one, 0xF2 does not).  Do not "fix" this.
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 64);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
                {
                    asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
                else
                {
                    // upstream: empty
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_TOSHIBA && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostReads = B8toINTle(asi.Attribute[j].RawValue);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_INTEL || asi.DiskVendorId == SSD_VENDOR_TOSHIBA || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION)
            {
                asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDFORCE || asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi.DiskVendorId == SSD_VENDOR_CORSAIR || asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_REALTEK
                || asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SSSTC || asi.DiskVendorId == SSD_VENDOR_SKHYNIX || asi.DiskVendorId == SSD_VENDOR_SEAGATE || asi.DiskVendorId == SSD_VENDOR_MARVELL
                || asi.DiskVendorId == SSD_VENDOR_MAXIOTEK || asi.DiskVendorId == SSD_VENDOR_YMTC || asi.DiskVendorId == SSD_VENDOR_SCY || asi.DiskVendorId == SSD_VENDOR_RECADATA || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03
                || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS || asi.DiskVendorId == SSD_VENDOR_SANDISK_DELL || asi.DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL
                )
            {
                if (asi.HostReadsWritesUnit == HOST_READS_WRITES_512B)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 2 / 1024 / 1024);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_16MB)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 64);
                }
                else if (asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB)
                {
                    asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                        /* upstream: B8toB64(...[0..5]) */
                        / 32); // 65536 * 512 / 1024 / 1024 / 1024;
                }
                else
                {
                    asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG || asi.DiskVendorId == SSD_VENDOR_JMICRON)
            {
                asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_PLEXTOR)
            {
                asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            {
                asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SANDISK)
            {
                asi.HostReads = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_MICRON || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xF3:
            if (asi.DiskVendorId == SSD_VENDOR_YMTC)
            {
                if (asi.Attribute[j].RawValue[0] > 0)
                {
                    asi.Temperature = asi.Attribute[j].RawValue[0];
                }

                if (asi.Temperature >= 100)
                {
                    asi.Temperature = -1000;
                }
            }
            else if (asi.DiskVendorId == SSD_VENDOR_INTEL)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            break;
        case 0xF9:
            if (asi.DiskVendorId == SSD_VENDOR_INTEL || asi.DiskVendorId == SSD_VENDOR_REALTEK || asi.DiskVendorId == SSD_VENDOR_WDC || (asi.DiskVendorId == SSD_VENDOR_SANDISK && asi.HostReadsWritesUnit == HOST_READS_WRITES_GB)
            || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS || asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS)
            {
                asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            else if (asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    * 16 / 1024 / 1024);
            }
            break;
        case 0xFA:
            if (asi.DiskVendorId == SSD_VENDOR_REALTEK)
            {
                asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            break;
        case 0x64:
            if (asi.DiskVendorId == SSD_VENDOR_SANDFORCE)
            {
                asi.GBytesErased = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            break;
        case 0xAD:
            if (asi.DiskVendorId == SSD_VENDOR_TOSHIBA || asi.DiskVendorId == SSD_VENDOR_KIOXIA)
            {
                asi.Life = asi.Attribute[j].CurrentValue - 100;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;

        case 0xB1:
            if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG)
            {
                asi.WearLevelingCount = B8toINTle(asi.Attribute[j].RawValue);
                // upstream: (INT)MAKELONG(MAKEWORD(RawValue[0], RawValue[1]),
                //                         MAKEWORD(RawValue[2], RawValue[3]));
                asi.Life = asi.Attribute[j].CurrentValue;
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xE7:
            if (asi.DiskVendorId == SSD_VENDOR_SANDFORCE || asi.DiskVendorId == SSD_VENDOR_CORSAIR || asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_SKHYNIX || asi.DiskVendorId == SSD_VENDOR_REALTEK
            ||  asi.DiskVendorId == SSD_VENDOR_SANDISK || asi.DiskVendorId == SSD_VENDOR_SSSTC || asi.DiskVendorId == SSD_VENDOR_APACER || asi.DiskVendorId == SSD_VENDOR_JMICRON || asi.DiskVendorId == SSD_VENDOR_PHISON
            ||  asi.DiskVendorId == SSD_VENDOR_SEAGATE || asi.DiskVendorId == SSD_VENDOR_MAXIOTEK || asi.DiskVendorId == SSD_VENDOR_YMTC || asi.DiskVendorId == SSD_VENDOR_SCY || asi.DiskVendorId == SSD_VENDOR_RECADATA || asi.DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL)
            {
                if (life.FlagLifeNoReport)
                {
                    asi.Life = -1;
                }
                else if (life.FlagLifeRawValueIncrement)
                {
                    asi.Life = 100 - asi.Attribute[j].RawValue[0];
                }
                else if (life.FlagLifeRawValue)
                {
                    asi.Life = asi.Attribute[j].RawValue[0];
                }
                else
                {
                    asi.Life = asi.Attribute[j].CurrentValue;
                }
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xA9:
            if (asi.DiskVendorId == SSD_VENDOR_REALTEK || (asi.DiskVendorId == SSD_VENDOR_KINGSTON && asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB) || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION)
            {
                if (life.FlagLifeRawValueIncrement)
                {
                    asi.Life = 100 - asi.Attribute[j].RawValue[0];
                }
                else if (life.FlagLifeRawValue)
                {
                    asi.Life = asi.Attribute[j].RawValue[0];
                }
                else
                {
                    asi.Life = asi.Attribute[j].CurrentValue;
                }
                if (asi.Life <= 0 || asi.Life > 100) { asi.Life = -1; }
            }
            break;
        case 0xC6:
            if (asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
            {
                asi.HostReads = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            break;
        case 0xC7:
            if (asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR)
            {
                asi.HostWrites = B8toINTle(asi.Attribute[j].RawValue); // upstream: (INT)(B8toB32(...))
            }
            break;
        case 0xF5:
            // Percent Drive Life Remaining (SanDisk/WD CloudSpeed)
            if (asi.DiskVendorId == SSD_VENDOR_SANDISK_CLOUD)
            {
                asi.Life = asi.Attribute[j].CurrentValue;
            }
            // NAND Page Size = 8KBytes
            // http://www.overclock.net/t/1145150/official-crucial-ssd-owners-club/1290
            else if (asi.DiskVendorId == SSD_VENDOR_MICRON)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    * 8 / 1024 / 1024);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_MICRON_MU03)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32);
            }
            else if (asi.DiskVendorId == SSD_VENDOR_KINGSTON && asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SILICONMOTION)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_SCY)
            {
                asi.NandWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 32); // 65536 * 512 / 1024 / 1024 / 1024;
            }
            else if (asi.DiskVendorId == SSD_VENDOR_RECADATA)
            {
                asi.NandWrites = B8toINTle(asi.Attribute[j].RawValue);
            }
            break;
        case 0xF6:
            if (asi.DiskVendorId == SSD_VENDOR_MICRON || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03)
            {
                asi.HostWrites = static_cast<INT>(detail::B8toB64le(asi.Attribute[j].RawValue)
                    /* upstream: B8toB64(...[0..5]) */
                    / 2 / 1024 / 1024);
            }
            break;
        default:
            break;
        }
    }
}

} // namespace cdi
