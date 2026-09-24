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
//  SmartCdiAdapter — see SmartCdiAdapter.h
// ═══════════════════════════════════════════════════════════════════════

#include "SmartCdiAdapter.h"
#include "CdiAttributeName.h"
#include "CdiSmartDetail.h"

#include <algorithm>

namespace smartcdi {

namespace {

std::string ToNarrowUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();

    const int len = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1,
                                          nullptr, 0, nullptr, nullptr);
    if (len <= 1) return std::string();

    std::string out(static_cast<size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, out.data(), len, nullptr, nullptr);
    return out;
}

// ── NVMe normalised columns ───────────────────────────────────────────
//
// CrystalDiskInfo's NVMe interpreter fills only Id and RawValue.  NVMe has no
// ATA-style normalised value / threshold page, so CurrentValue, WorstValue and
// Threshold stay zero and the attribute table renders three empty columns —
// only Raw would show anything.
//
// The values below restore what the previous (smartmontools-based) NVMe reader
// produced: 100 = healthy, 0 = fault, and a threshold wherever the log field is
// a pass/fail or an advisory signal.  Every other NVMe field is an
// informational counter, which reads as 100 / 100 / 0.
//
// The ids are CrystalDiskInfo's, i.e. the ones the NVMe interpreter writes —
// see the id table in src/CdiSmartNvmeInterp.cpp.
void NormaliseNvmeAttributes(std::vector<SmartAttribute>& attrs) {
    // Available Spare is judged against the Available Spare Threshold, which
    // the log carries as its own field.
    uint8_t spareThreshold = 0;
    for (const SmartAttribute& a : attrs) {
        if (a.id == 0x04) {
            spareThreshold = static_cast<uint8_t>(std::min<uint64_t>(a.rawValue, 100));
        }
    }

    for (SmartAttribute& a : attrs) {
        a.current = 100;
        a.worst = 100;
        a.threshold = 0;
        a.preFailure = false;

        switch (a.id) {
        case 0x01:  // Critical Warning — any set bit is a fault
            a.current = a.worst = (a.rawValue == 0) ? 100 : 0;
            a.threshold = 1;
            a.preFailure = true;
            break;

        case 0x03:  // Available Spare: percent remaining, vs. its own threshold
            a.current = a.worst = static_cast<uint8_t>(std::min<uint64_t>(a.rawValue, 100));
            a.threshold = spareThreshold;
            a.preFailure = true;
            break;

        case 0x05:  // Percentage Used — inverted so it reads as remaining life
            a.current = a.worst =
                (a.rawValue <= 100) ? static_cast<uint8_t>(100 - a.rawValue) : 0;
            break;

        case 0x0E:  // Media and Data Integrity Errors — any count is a fault
            a.current = a.worst = (a.rawValue == 0) ? 100 : 0;
            a.threshold = 1;
            a.preFailure = true;
            break;

        default:
            break;
        }
    }
}

} // anonymous namespace

// ── Interface mapping ─────────────────────────────────────────────────

DiskInterfaceType ToDiskInterfaceType(const cdi::DRIVE_INFO& asi) {
    // IsNVMe is the authoritative flag: AddDiskNVMe() sets it and the
    // SMART log fields it parses are the NVMe ones.  InterfaceType alone
    // would put an NVMe drive behind a USB bridge into the USB bucket.
    if (asi.IsNVMe) return DiskInterfaceType::NVMe;

    switch (asi.InterfaceType) {
    case cdi::INTERFACE_TYPE_NVME: return DiskInterfaceType::NVMe;
    case cdi::INTERFACE_TYPE_PATA:
    case cdi::INTERFACE_TYPE_SATA: return DiskInterfaceType::ATA;
    case cdi::INTERFACE_TYPE_USB:
    case cdi::INTERFACE_TYPE_IEEE1394:
    case cdi::INTERFACE_TYPE_SCSI:
    case cdi::INTERFACE_TYPE_AMD_RC2: return DiskInterfaceType::SCSI;
    default: break;
    }

    // A disk with ATA attributes but no bus information is still ATA.
    if (asi.AttributeCount > 0 && !asi.IsNVMe) return DiskInterfaceType::ATA;
    return DiskInterfaceType::Unknown;
}

std::wstring InterfaceName(const cdi::DRIVE_INFO& asi) {
    if (!asi.Interface.empty()) return asi.Interface;

    switch (asi.InterfaceType) {
    case cdi::INTERFACE_TYPE_NVME: return L"NVM Express";
    case cdi::INTERFACE_TYPE_SATA: return L"Serial ATA";
    case cdi::INTERFACE_TYPE_PATA: return L"Parallel ATA";
    case cdi::INTERFACE_TYPE_USB: return L"USB";
    case cdi::INTERFACE_TYPE_IEEE1394: return L"IEEE 1394";
    case cdi::INTERFACE_TYPE_SCSI: return L"SCSI";
    case cdi::INTERFACE_TYPE_AMD_RC2: return L"AMD RC2";
    default: break;
    }
    return L"Unknown";
}

// ── Identity ──────────────────────────────────────────────────────────

DiskIdentity MakeIdentity(const cdi::DRIVE_INFO& asi, uint32_t diskNumber) {
    DiskIdentity id;
    id.diskNumber = diskNumber;
    id.model = asi.Model;
    id.serialNumber = asi.SerialNumber;
    id.firmwareRevision = asi.FirmwareRev;

    id.sectorSize = asi.LogicalSectorSize != 0 ? asi.LogicalSectorSize : 512;

    // NumberOfSectors is the figure CrystalDiskInfo settles on after its
    // 28-bit / 48-bit LBA reconciliation, so it is the best capacity source.
    const uint64_t sectors = static_cast<uint64_t>(asi.NumberOfSectors);
    if (sectors != 0) {
        id.capacityBytes = sectors * id.sectorSize;
    } else if (asi.TotalDiskSize != 0) {
        // TotalDiskSize is in 10^6 bytes (CrystalDiskInfo divides by 1000 twice).
        id.capacityBytes = static_cast<uint64_t>(asi.TotalDiskSize) * 1000000ULL;
    } else {
        id.capacityBytes = 0;
    }

    id.diskInterface = ToDiskInterfaceType(asi);
    id.smartSupported = asi.IsSmartSupported != FALSE;
    id.smartEnabled = asi.IsSmartEnabled != FALSE;
    id.interfaceName = ToNarrowUtf8(InterfaceName(asi));

    // 0 = unknown, 1 = SSD, > 1 = rotational speed in rpm.
    if (asi.IsSsd || asi.NominalMediaRotationRate == 1) {
        id.rotationRate = 1;
    } else if (asi.NominalMediaRotationRate > 1) {
        id.rotationRate = static_cast<int>(asi.NominalMediaRotationRate);
    } else {
        id.rotationRate = 0;
    }

    return id;
}

// ── Attributes ────────────────────────────────────────────────────────

uint64_t RawValueOf(const cdi::SMART_ATTRIBUTE& attr) {
    // Every multi-byte raw field CrystalDiskInfo writes — the ATA ones read
    // straight from the 512 byte SMART page and the NVMe ones produced by
    // the interpreter — is little endian in RawValue[6].  The 16-bit and
    // 32-bit fields simply have their high bytes zero.
    return cdi::detail::B8toB64le(attr.RawValue);
}

std::vector<SmartAttribute> MakeAttributes(const cdi::DRIVE_INFO& asi) {
    std::vector<SmartAttribute> out;
    out.reserve(asi.AttributeCount);

    const DWORD count = std::min<DWORD>(asi.AttributeCount, cdi::MAX_ATTRIBUTE);
    for (DWORD i = 0; i < count; ++i) {
        const cdi::SMART_ATTRIBUTE& src = asi.Attribute[i];
        if (src.Id == 0) continue;   // empty slot

        SmartAttribute attr;
        attr.id = src.Id;
        attr.name = ToNarrowUtf8(cdi::GetSmartAttributeName(asi, src.Id));
        attr.current = src.CurrentValue;
        attr.worst = src.WorstValue;
        attr.rawValue = RawValueOf(src);
        attr.rawString = ToNarrowUtf8(cdi::FormatSmartRawValue(asi, src));

        // Attribute status flags, per SFF-8035i: bit 0 is the pre-failure
        // advisory bit.  This is the only flag the GUI reads.
        attr.preFailure = (src.StatusFlags & 0x0001) != 0;
        attr.online = (src.StatusFlags & 0x0002) != 0;

        const cdi::SMART_THRESHOLD& thr = asi.Threshold[i];
        attr.threshold = (thr.Id == src.Id) ? thr.ThresholdValue : 0;

        out.push_back(attr);
    }

    // NVMe drives get their normalised columns synthesised; ATA drives carry
    // real ones, parsed from the SMART values / thresholds pages above.
    if (asi.IsNVMe) {
        NormaliseNvmeAttributes(out);
    }

    return out;
}

// ── Health verdict ────────────────────────────────────────────────────

int SmartReturnStatus(const cdi::DRIVE_INFO& asi) {
    // CrystalDiskInfo's CheckDiskStatus() is the same overall-health verdict
    // its own GUI shows, and it is computed during both Init() and every
    // UpdateSmartInfo(), so no extra device round-trip is needed.
    switch (asi.DiskStatus) {
    case cdi::DISK_STATUS_GOOD:    return 0;
    case cdi::DISK_STATUS_CAUTION:
    case cdi::DISK_STATUS_BAD:     return 1;
    default:                       return -1;
    }
}

// ── Snapshot ──────────────────────────────────────────────────────────

void FillSnapshot(const cdi::DRIVE_INFO& asi, uint32_t diskNumber,
                  SmartDataSnapshot& snapshot) {
    snapshot.identity = MakeIdentity(asi, diskNumber);
    snapshot.attributes = MakeAttributes(asi);

    // -1000 is CrystalDiskInfo's "no reading" sentinel.
    snapshot.temperatureCelsius = (asi.Temperature <= -1000) ? 0.0
                                                             : static_cast<double>(asi.Temperature);

    // Byte totals come straight from the acquisition layer: the ATA path
    // scales per vendor inside FillSmartData(), the NVMe path in
    // AddDiskNVMe()/UpdateSmartInfo(), and both land in the *Bytes fields.
    snapshot.totalBytesRead = asi.HostReadsBytes;
    snapshot.totalBytesWritten = asi.HostWritesBytes;

    // Kept for the debug view; the byte totals above are what the UI uses.
    snapshot.totalLbasRead = 0;
    snapshot.totalLbasWritten = 0;

    snapshot.powerOnHours = (asi.PowerOnHours > 0)
                                ? static_cast<uint64_t>(asi.PowerOnHours) : 0;

    snapshot.wearLevelingCount = asi.WearLevelingCount;
    snapshot.remainingLifePercent = asi.Life;

    snapshot.smartReturnStatus = SmartReturnStatus(asi);

    snapshot.dataValid = true;
    snapshot.errorMessage.clear();
    snapshot.permissionHint.clear();
}

} // namespace smartcdi
