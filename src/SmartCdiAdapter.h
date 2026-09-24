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

// ═══════════════════════════════════════════════════════════════════════
//  SmartCdiAdapter — cdi::DRIVE_INFO -> the GUI's SMART data model
//
//  The acquisition layer is a port of CrystalDiskInfo's CAtaSmart and works
//  in terms of cdi::DRIVE_INFO (see CdiSmart.h).  Everything that draws or
//  grades a drive works in terms of SmartDataModel.h's DiskIdentity /
//  SmartAttribute / SmartDataSnapshot.  This is the only place the two meet,
//  which keeps SmartMonitor, SmartOverlayWindow, SmartDebugWindow and the
//  rendering code free of CrystalDiskInfo types.
//
//  Fields are taken from DRIVE_INFO directly rather than re-derived from the
//  attribute list: CrystalDiskInfo remaps NVMe log fields onto ATA-style
//  attribute ids (0x01..0x1D), so the smartmontools-era "look for id 194 /
//  241 / 242" extraction does not hold for NVMe drives.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmart.h"
#include "SmartDataModel.h"

#include <vector>

namespace smartcdi {

// cdi::INTERFACE_TYPE / COMMAND_TYPE -> the GUI's three-way enum.
// computeHealth() branches on DiskInterfaceType::NVMe, so NVMe detection
// has to survive the mapping.
DiskInterfaceType ToDiskInterfaceType(const cdi::DRIVE_INFO& asi);

// Human readable bus name, e.g. "NVM Express", "Serial ATA", "USB".
std::wstring InterfaceName(const cdi::DRIVE_INFO& asi);

// DiskIdentity from the identify data.  diskNumber is the caller's own
// enumeration index (SmartMonitor numbers disks by position).
DiskIdentity MakeIdentity(const cdi::DRIVE_INFO& asi, uint32_t diskNumber);

// The raw value column for one attribute.  CrystalDiskInfo stores every
// multi-byte raw field little endian in SMART_ATTRIBUTE::RawValue[6], so a
// single 48-bit read covers the ATA attributes and the NVMe interpreter's
// remapped fields alike.
uint64_t RawValueOf(const cdi::SMART_ATTRIBUTE& attr);

// Attribute table for the GUI, using the CrystalDiskInfo naming and raw
// value presentation already implemented in CdiAttributeName.h.
std::vector<SmartAttribute> MakeAttributes(const cdi::DRIVE_INFO& asi);

// S.M.A.R.T. RETURN STATUS style health verdict:
//   0 = good, 1 = threshold exceeded / caution+ / bad, -1 = not available.
// Derived from CrystalDiskInfo's CheckDiskStatus() result rather than a
// second device round-trip.
int SmartReturnStatus(const cdi::DRIVE_INFO& asi);

// Everything SmartDataSnapshot carries, filled from DRIVE_INFO.
// diskNumber is the caller's own enumeration index.  Leaves the rate fields,
// the session fields, the status grade and the chart bookkeeping to the
// caller, which owns the sampling clock and the health algorithm.
void FillSnapshot(const cdi::DRIVE_INFO& asi, uint32_t diskNumber,
                  SmartDataSnapshot& snapshot);

} // namespace smartcdi
