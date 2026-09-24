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
//  CdiAttributeName — S.M.A.R.T. attribute naming / raw value presentation
//
//  CrystalDiskInfo stores attribute names in its language files
//  ([Smart], [SmartSsd], [SmartNVMe] and the [Smart<Vendor>] sections) and
//  selects the section through ATA_SMART_INFO::SmartKeyName.  The tables
//  were extracted verbatim into CdiSmartAttributeTable.cpp so the same
//  naming is produced here.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmart.h"
#include <string>

namespace cdi {

struct CdiAttrName {
    BYTE        id;
    const char* name;
};

struct CdiAttrTable {
    const char*        key;
    const CdiAttrName* entries;
    int                count;
};

// Provided by the generated CdiSmartAttributeTable.cpp
bool CdiFindAttrTable(const char* key, const CdiAttrName** entries, int* count);

// CrystalDiskInfo CAtaSmart::CheckSsdSupport() assigns SmartKeyName, which
// then drives attribute naming.  Sets asi.SmartKeyName.
void SetSmartKeyName(DRIVE_INFO& asi);

// CrystalDiskInfo attribute name lookup (language file replacement).
std::wstring GetSmartAttributeName(const DRIVE_INFO& asi, BYTE id);

// Human readable "raw value" column, following CrystalDiskInfo's
// UpdateListCtrl() presentation rules (hex / decimal dumps plus the
// computed temperature, life and power-on-hours presentations).
std::wstring FormatSmartRawValue(const DRIVE_INFO& asi,
                                 const SMART_ATTRIBUTE& attr);

// Index of an attribute inside DRIVE_INFO::Attribute, or -1.
int GetSmartAttributeIndex(const DRIVE_INFO& asi, BYTE id);

} // namespace cdi
