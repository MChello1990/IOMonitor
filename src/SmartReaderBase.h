#pragma once

#include "SmartDataModel.h"
#include <string>
#include <memory>
#include <vector>

// ═══════════════════════════════════════════════════════════════════════
//  SmartReaderBase — Abstract interface modeled after smartmontools'
//  ata_device + nvme_device pattern.
//
//  Provides unified access across ATA, NVMe, and SCSI protocols.
//  Each concrete implementation handles the platform-specific
//  IOCTL passthrough and data parsing exactly as smartmontools does.
// ═══════════════════════════════════════════════════════════════════════

class SmartReaderBase {
public:
    virtual ~SmartReaderBase() = default;

    // Open a handle to the physical disk
    virtual bool open(uint32_t diskNumber) = 0;

    // Close the handle and release resources
    virtual void close() = 0;

    // Check if the disk is currently open
    virtual bool isOpen() const = 0;

    // Retrieve disk identity information (model, serial, firmware, capacity)
    virtual bool readIdentity(DiskIdentity& identity) = 0;

    // Read all SMART attributes (ATA: 30 attributes; NVMe: health log fields)
    virtual bool readAttributes(std::vector<SmartAttribute>& attributes) = 0;

    // Read SMART threshold values (ATA only; NVMe has implicit thresholds)
    virtual bool readThresholds(std::vector<SmartAttribute>& attributes) = 0;

    // SMART RETURN STATUS check — smartmontools' ataSmartStatus2()
    // Returns: 0 = good (no threshold exceeded), 1 = failing, -1 = error/unsupported
    virtual int checkSmartStatus() { return -1; }

    // Get interface type this reader handles
    virtual DiskInterfaceType interfaceType() const = 0;

    // Human-readable interface name
    virtual std::string interfaceName() const = 0;

    // Get the raw device path for diagnostic purposes
    virtual std::wstring devicePath() const = 0;

protected:
    // Build the device path for a given physical drive number
    static std::wstring buildDevicePath(uint32_t diskNumber) {
        wchar_t buf[64];
        swprintf(buf, 64, L"\\\\.\\PhysicalDrive%u", diskNumber);
        return buf;
    }

    // ── ATA/SATA constants (smartmontools exact match) ──────────────────
    static constexpr uint8_t ATA_SMART_CMD             = 0xB0;
    static constexpr uint8_t ATA_SMART_READ_DATA       = 0xD0;
    static constexpr uint8_t ATA_SMART_READ_THRESHOLDS = 0xD1;
    static constexpr uint8_t ATA_SMART_ENABLE          = 0xD8;
    static constexpr uint8_t ATA_SMART_STATUS          = 0xDA;
    static constexpr uint8_t ATA_SMART_READ_LOG        = 0xD5;
    static constexpr uint8_t ATA_IDENTIFY_DEVICE       = 0xEC;
    static constexpr uint8_t ATA_IDENTIFY_PACKET       = 0xA1;
    static constexpr uint8_t ATA_READ_LOG_EXT          = 0x2F;

    static constexpr uint8_t SMART_CYL_LOW  = 0x4F;
    static constexpr uint8_t SMART_CYL_HI   = 0xC2;

    // ── NVMe constants ──────────────────────────────────────────────────
    static constexpr uint8_t NVME_ADMIN_GET_LOG_PAGE = 0x02;
    static constexpr uint8_t NVME_ADMIN_IDENTIFY     = 0x06;

    // ── Parse ATA SMART data (512-byte sector) into attributes ─────────
    static void parseAtaSmartData(const uint8_t* data,
                                  std::vector<SmartAttribute>& attributes);

    // ── Parse ATA SMART thresholds (512-byte sector) ───────────────────
    static void parseAtaThresholds(const uint8_t* data,
                                   std::vector<SmartAttribute>& attributes);

    // ── Parse NVMe SMART / Health Information Log (512-byte) ───────────
    static void parseNvmeSmartLog(const uint8_t* data,
                                  std::vector<SmartAttribute>& attributes);

    // ── Parse ATA IDENTIFY DEVICE data ─────────────────────────────────
    static void parseAtaIdentify(const uint8_t* data, DiskIdentity& identity,
                                 uint32_t diskNumber);

    // ── Detect rotation rate from IDENTIFY data ────────────────────────
    static int detectRotationRate(const AtaIdentifyDevice* id);

    // ── Check if SMART is supported/enabled from IDENTIFY data ─────────
    static bool isSmartSupported(const AtaIdentifyDevice* drive);
    static int isSmartEnabled(const AtaIdentifyDevice* drive);
};

// ═══════════════════════════════════════════════════════════════════════
//  Factory function to detect and create the correct reader for a disk
// ═══════════════════════════════════════════════════════════════════════

std::unique_ptr<SmartReaderBase> createSmartReader(uint32_t diskNumber);
