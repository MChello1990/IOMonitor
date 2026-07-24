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

#include "SmartReaderBase.h"

// ═══════════════════════════════════════════════════════════════════════
//  SmartReaderAta — S.M.A.R.T. data retrieval for ATA/SATA devices
//
//  Implements smartmontools' multi-path approach:
//    1. IOCTL_ATA_PASS_THROUGH (primary)
//    2. IOCTL_SCSI_MINIPORT with SMART_RCV_DRIVE_DATA (fallback)
//    3. SAT (SCSI/ATA Translation) for USB bridges
//
//  ATA commands follow smartmontools' smartcommandhandler() pattern.
// ═══════════════════════════════════════════════════════════════════════

class SmartReaderAta : public SmartReaderBase {
public:
    SmartReaderAta();
    ~SmartReaderAta() override;

    bool open(uint32_t diskNumber) override;
    void close() override;
    bool isOpen() const override { return m_hDevice != INVALID_HANDLE_VALUE; }

    bool readIdentity(DiskIdentity& identity) override;
    bool readAttributes(std::vector<SmartAttribute>& attributes) override;
    bool readThresholds(std::vector<SmartAttribute>& attributes) override;
    int checkSmartStatus() override;

    DiskInterfaceType interfaceType() const override { return DiskInterfaceType::ATA; }
    std::string interfaceName() const override { return "ATA"; }
    std::wstring devicePath() const override { return m_devicePath; }

private:
    // ── ATA Pass-Through (primary path) ────────────────────────────────
    // Send an ATA command via IOCTL_ATA_PASS_THROUGH
    // Mirrors smartmontools' ata_pass_through() device interface
    bool sendAtaPassThrough(uint8_t command, uint8_t feature,
                            uint8_t lbaLow, uint8_t lbaMid, uint8_t lbaHigh,
                            uint8_t device, uint8_t sectorCount,
                            uint8_t* dataBuffer, uint32_t dataSize,
                            bool readData);

    // ── SCSI Miniport (fallback path) ──────────────────────────────────
    // Send SMART_RCV_DRIVE_DATA via IOCTL_SCSI_MINIPORT
    // Compatible with storport/storahci drivers that block ATA_PASS_THROUGH
    bool sendSmartRcvDriveData(uint8_t subCommand, uint32_t driveNumber,
                                uint8_t* dataBuffer, uint32_t dataSize);

    // ── SAT (SCSI/ATA Translation) path ────────────────────────────────
    // For USB bridges that support the SAT protocol
    bool sendSatPassThrough(uint8_t ataCommand, uint8_t feature,
                            uint8_t lbaLow, uint8_t lbaMid, uint8_t lbaHigh,
                            uint8_t device, uint8_t sectorCount,
                            uint8_t* dataBuffer, uint32_t dataSize,
                            bool readData);

    // ── STORAGE_PROPERTY_QUERY fallback ────────────────────────────────
    bool readIdentityViaStorageQuery(DiskIdentity& identity);

    // ── SMART ENABLE command ───────────────────────────────────────────
    bool enableSmart();

    // ── SMART RETURN STATUS — smartmontools' ataSmartStatus2() ────────
    // Returns: 0 = good, 1 = failing, -1 = error
    int smartStatusCheck();

    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    std::wstring m_devicePath;
    uint32_t m_diskNumber = 0;

    // Path capability flags
    bool m_ataPassThroughOk = false;
    bool m_scsiMiniportOk = false;
    bool m_satOk = false;

    // Primary path for data commands
    enum class DataPath { ATA_PT, SCSI_MINIPORT, SAT };
    DataPath m_dataPath = DataPath::ATA_PT;
};
