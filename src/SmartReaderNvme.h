#pragma once

#include "SmartReaderBase.h"

// ═══════════════════════════════════════════════════════════════════════
//  SmartReaderNvme — S.M.A.R.T. / Health Information for NVMe devices
//
//  Implements smartmontools' nvme_device interface:
//    - IOCTL_STORAGE_PROTOCOL_COMMAND for NVMe pass-through
//    - Fallback: IOCTL_STORAGE_QUERY_PROPERTY with ProtocolSpecificProperty
//    - NVMe Admin Identify (CNS=0x01) for controller identity
//    - NVMe Get Log Page (LID=0x02) for SMART/Health Information
//
//  Uses smartmontools' nvme_smart_log structure for exact parsing.
// ═══════════════════════════════════════════════════════════════════════

class SmartReaderNvme : public SmartReaderBase {
public:
    SmartReaderNvme();
    ~SmartReaderNvme() override;

    bool open(uint32_t diskNumber) override;
    void close() override;
    bool isOpen() const override { return m_hDevice != INVALID_HANDLE_VALUE; }

    bool readIdentity(DiskIdentity& identity) override;
    bool readAttributes(std::vector<SmartAttribute>& attributes) override;
    bool readThresholds(std::vector<SmartAttribute>& attributes) override;
    int checkSmartStatus() override;

    DiskInterfaceType interfaceType() const override { return DiskInterfaceType::NVMe; }
    std::string interfaceName() const override { return "NVMe"; }
    std::wstring devicePath() const override { return m_devicePath; }

private:
    // ── NVMe pass-through via IOCTL_STORAGE_PROTOCOL_COMMAND ──────────
    // Sends an NVMe admin command and retrieves data
    bool sendNvmeCommand(uint8_t opcode, uint32_t nsid,
                         uint32_t cdw10, uint32_t cdw11,
                         uint8_t* dataBuffer, uint32_t dataSize,
                         bool readData);

    // ── Fallback: STORAGE_PROTOCOL_SPECIFIC_DATA ──────────────────────
    // Uses IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceProtocolSpecificProperty
    // to retrieve NVMe SMART/Health log page
    bool readSmartLogViaStorageQuery(uint8_t* data, uint32_t dataSize);

    // ── Fallback: STORAGE_PREDICT_FAILURE ─────────────────────────────
    bool readSmartLogViaPredictFailure(uint8_t* data, uint32_t dataSize);

    HANDLE m_hDevice = INVALID_HANDLE_VALUE;
    std::wstring m_devicePath;
    uint32_t m_diskNumber = 0;
    bool m_protocolCommandOk = false;
};
