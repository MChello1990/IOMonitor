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
//  SmartReaderScsi — S.M.A.R.T. retrieval for SCSI / USB-bridged disks
//
//  Implements smartmontools' SCSI pass-through approach:
//    1. SAT (SCSI/ATA Translation) — ATA PASS-THROUGH (16) for USB bridges
//    2. SCSI LOG SENSE — for temperature and SMART-equivalent data
//    3. STORAGE_DEVICE_DESCRIPTOR — for basic identity
//
//  Uses IOCTL_SCSI_PASS_THROUGH for SCSI command submission.
// ═══════════════════════════════════════════════════════════════════════

class SmartReaderScsi : public SmartReaderBase {
public:
    SmartReaderScsi();
    ~SmartReaderScsi() override;

    bool open(uint32_t diskNumber) override;
    void close() override;
    bool isOpen() const override { return m_hDevice != INVALID_HANDLE_VALUE; }

    bool readIdentity(DiskIdentity& identity) override;
    bool readAttributes(std::vector<SmartAttribute>& attributes) override;
    bool readThresholds(std::vector<SmartAttribute>& attributes) override;
    int checkSmartStatus() override;

    DiskInterfaceType interfaceType() const override { return DiskInterfaceType::SCSI; }
    std::string interfaceName() const override { return "SCSI/USB"; }
    std::wstring devicePath() const override { return m_devicePath; }

private:
    // ── SCSI pass-through ──────────────────────────────────────────────
    bool sendScsiCommand(const uint8_t* cdb, uint8_t cdbLength,
                          uint8_t* dataBuffer, uint32_t dataSize,
                          bool readData);

    // ── SAT (SCSI/ATA Translation) ATA PASS-THROUGH (16) ───────────────
    bool sendSatAtaPassThrough(uint8_t ataCommand, uint8_t feature,
                                uint8_t lbaLow, uint8_t lbaMid, uint8_t lbaHigh,
                                uint8_t device, uint8_t sectorCount,
                                uint8_t* dataBuffer, uint32_t dataSize,
                                bool readData);

    // ── SCSI INQUIRY ──────────────────────────────────────────────────
    bool sendScsiInquiry(uint8_t page, uint8_t* data, uint32_t dataSize);

    // ── SCSI LOG SENSE ─────────────────────────────────────────────────
    bool sendScsiLogSense(uint8_t page, uint8_t subpage,
                           uint8_t* data, uint32_t dataSize);

    // ── Probe SAT support ─────────────────────────────────────────────
    bool probeSatSupport();

    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    std::wstring m_devicePath;
    uint32_t m_diskNumber = 0;
    bool m_satAvailable = false;
};
