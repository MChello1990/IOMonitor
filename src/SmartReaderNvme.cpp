#include "SmartReaderNvme.h"
#include <winioctl.h>
#include <ntddscsi.h>
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ntdll.lib")
#endif

// ── NVMe pass-through via IOCTL_STORAGE_PROTOCOL_COMMAND ─────────────
// Use local names to avoid SDK conflicts
#ifndef NVME_LOCAL_PROTOCOL_TYPE
#define NVME_LOCAL_PROTOCOL_TYPE 0x03
#endif

#ifndef NVME_LOCAL_CMD_LENGTH
#define NVME_LOCAL_CMD_LENGTH 64
#endif

namespace {

// Local NVMe protocol command structure (avoid SDK name conflicts)
struct NvmeProtocolCommand {
    DWORD   Version;
    DWORD   Length;
    DWORD   ProtocolType;
    DWORD   Flags;
    DWORD   ReturnStatus;
    DWORD   ErrorCode;
    DWORD   CommandLength;
    DWORD   ErrorInfoLength;
    DWORD   DataToDeviceTransferLength;
    DWORD   DataFromDeviceTransferLength;
    DWORD   TimeOutValue;
    DWORD   ErrorInfoOffset;
    DWORD   DataToDeviceBufferOffset;
    DWORD   DataFromDeviceBufferOffset;
    DWORD   CommandSpecific;
    DWORD   Reserved0;
    DWORD   FixedProtocolReturnData;
    DWORD   Reserved1[3];
    UCHAR   Command[NVME_LOCAL_CMD_LENGTH];
};

// Local NVMe protocol-specific data for fallback
struct NvmeProtocolSpecificData {
    DWORD   ProtocolType;
    DWORD   DataType;
    DWORD   ProtocolDataRequestValue;
    DWORD   ProtocolDataRequestSubValue;
    DWORD   ProtocolDataOffset;
    DWORD   ProtocolDataLength;
    DWORD   FixedProtocolReturnData;
    DWORD   Reserved[3];
};

// NVMe Data Type for SMART/Health Information
constexpr DWORD NVME_DATA_TYPE_SMART = 0x0002;

// IOCTL code
#ifndef IOCTL_STORAGE_PROTOCOL_COMMAND
#define IOCTL_STORAGE_PROTOCOL_COMMAND \
    CTL_CODE(IOCTL_STORAGE_BASE, 0x04F0, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════

SmartReaderNvme::SmartReaderNvme() {}
SmartReaderNvme::~SmartReaderNvme() { close(); }

// ═══════════════════════════════════════════════════════════════════════
//  Open device
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::open(uint32_t diskNumber) {
    close();
    m_diskNumber = diskNumber;
    m_devicePath = buildDevicePath(diskNumber);
    m_protocolCommandOk = false;

    m_hDevice = CreateFileW(
        m_devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    // Probe NVMe pass-through with a small read
    uint8_t probeBuf[sizeof(NvmeSmartLog)] = {};
    m_protocolCommandOk = sendNvmeCommand(
        NVME_ADMIN_GET_LOG_PAGE, 0xFFFFFFFF,
        0x02 | (((sizeof(NvmeSmartLog) / 4) - 1) << 16),
        0, probeBuf, sizeof(NvmeSmartLog), true);

    return true;
}

void SmartReaderNvme::close() {
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
    m_devicePath.clear();
}

// ═══════════════════════════════════════════════════════════════════════
//  NVMe pass-through via IOCTL_STORAGE_PROTOCOL_COMMAND
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::sendNvmeCommand(uint8_t opcode, uint32_t nsid,
                                       uint32_t cdw10, uint32_t cdw11,
                                       uint8_t* dataBuffer, uint32_t dataSize,
                                       bool readData) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    DWORD totalSize = sizeof(NvmeProtocolCommand) + dataSize;
    std::vector<uint8_t> buffer(totalSize, 0);

    auto* cmd = reinterpret_cast<NvmeProtocolCommand*>(buffer.data());
    cmd->Version = sizeof(NvmeProtocolCommand);
    cmd->Length = sizeof(NvmeProtocolCommand);
    cmd->ProtocolType = NVME_LOCAL_PROTOCOL_TYPE;
    cmd->Flags = 0;
    cmd->CommandLength = NVME_LOCAL_CMD_LENGTH;
    cmd->ErrorInfoLength = 0;
    cmd->TimeOutValue = 10;

    if (readData) {
        cmd->DataFromDeviceTransferLength = dataSize;
        cmd->DataFromDeviceBufferOffset = sizeof(NvmeProtocolCommand);
    } else {
        cmd->DataToDeviceTransferLength = dataSize;
        cmd->DataToDeviceBufferOffset = sizeof(NvmeProtocolCommand);
    }

    // Build NVMe command: full 64-byte command per NVMe spec
    // Matches smartmontools' NVME_COMMAND structure layout:
    //   Bytes 0-3:   CDW0  (OPC in bits 0-7)
    //   Bytes 4-7:   NSID
    //   Bytes 8-23:  Reserved (CDW2-CDW5)
    //   Bytes 24-39: Metadata Pointer (CDW6-CDW9)
    //   Bytes 40-43: CDW10
    //   Bytes 44-47: CDW11
    //   Bytes 48-51: CDW12
    //   Bytes 52-55: CDW13
    //   Bytes 56-59: CDW14
    //   Bytes 60-63: CDW15
    memset(cmd->Command, 0, NVME_LOCAL_CMD_LENGTH);
    cmd->Command[0] = opcode;  // CDW0: Opcode
    // NSID in bytes 4-7 (little-endian)
    cmd->Command[4] = (uint8_t)(nsid & 0xFF);
    cmd->Command[5] = (uint8_t)((nsid >> 8) & 0xFF);
    cmd->Command[6] = (uint8_t)((nsid >> 16) & 0xFF);
    cmd->Command[7] = (uint8_t)((nsid >> 24) & 0xFF);
    // CDW10 in bytes 40-43 (little-endian)
    cmd->Command[40] = (uint8_t)(cdw10 & 0xFF);
    cmd->Command[41] = (uint8_t)((cdw10 >> 8) & 0xFF);
    cmd->Command[42] = (uint8_t)((cdw10 >> 16) & 0xFF);
    cmd->Command[43] = (uint8_t)((cdw10 >> 24) & 0xFF);
    // CDW11 in bytes 44-47 (little-endian)
    cmd->Command[44] = (uint8_t)(cdw11 & 0xFF);
    cmd->Command[45] = (uint8_t)((cdw11 >> 8) & 0xFF);
    cmd->Command[46] = (uint8_t)((cdw11 >> 16) & 0xFF);
    cmd->Command[47] = (uint8_t)((cdw11 >> 24) & 0xFF);
    // CDW12-CDW15 remain zero (not used for Get Log Page or Identify)

    if (!readData && dataSize > 0) {
        memcpy(buffer.data() + sizeof(NvmeProtocolCommand), dataBuffer, dataSize);
    }

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_STORAGE_PROTOCOL_COMMAND,
        buffer.data(), totalSize,
        buffer.data(), totalSize,
        &bytesReturned, nullptr);

    if (!ok) return false;

    if (readData && dataSize > 0) {
        memcpy(dataBuffer, buffer.data() + sizeof(NvmeProtocolCommand), dataSize);
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Read SMART log via STORAGE_PROTOCOL_SPECIFIC_DATA (fallback)
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::readSmartLogViaStorageQuery(uint8_t* data, uint32_t dataSize) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProtocolSpecificProperty;
    query.QueryType = PropertyStandardQuery;

    // Build protocol-specific request
    NvmeProtocolSpecificData psd = {};
    psd.ProtocolType = NVME_LOCAL_PROTOCOL_TYPE;
    psd.DataType = NVME_DATA_TYPE_SMART;
    psd.ProtocolDataRequestValue = 2;
    psd.ProtocolDataRequestSubValue = 0;
    psd.ProtocolDataOffset = sizeof(NvmeProtocolSpecificData);
    psd.ProtocolDataLength = dataSize;

    // Buffer layout:
    // [STORAGE_PROPERTY_QUERY header fields (no padding)]
    // [NvmeProtocolSpecificData via AdditionalParameters flexible array]
    // [Response data]
    DWORD headerSize = FIELD_OFFSET(STORAGE_PROPERTY_QUERY, AdditionalParameters);
    DWORD totalSize = headerSize + sizeof(NvmeProtocolSpecificData) + dataSize;
    std::vector<uint8_t> buffer(totalSize, 0);

    memcpy(buffer.data(), &query, headerSize);
    memcpy(buffer.data() + headerSize, &psd, sizeof(psd));

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_STORAGE_QUERY_PROPERTY,
        buffer.data(), totalSize,
        buffer.data(), totalSize,
        &bytesReturned, nullptr);

    if (!ok) return false;

    DWORD responseOffset = headerSize + sizeof(NvmeProtocolSpecificData);
    if (bytesReturned >= responseOffset + dataSize) {
        memcpy(data, buffer.data() + responseOffset, dataSize);
        return true;
    }

    return false;
}

// ═══════════════════════════════════════════════════════════════════════
//  Read SMART log via STORAGE_PREDICT_FAILURE (last resort fallback)
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::readSmartLogViaPredictFailure(uint8_t* data, uint32_t dataSize) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    STORAGE_PREDICT_FAILURE pred = {};
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_STORAGE_PREDICT_FAILURE,
        nullptr, 0,
        &pred, sizeof(pred),
        &bytesReturned, nullptr);

    if (!ok) return false;

    uint32_t copySize = (dataSize < (uint32_t)sizeof(pred.VendorSpecific))
                        ? dataSize : (uint32_t)sizeof(pred.VendorSpecific);
    memcpy(data, pred.VendorSpecific, copySize);

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  Read Identity
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::readIdentity(DiskIdentity& identity) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    identity.diskNumber = m_diskNumber;
    identity.diskInterface = DiskInterfaceType::NVMe;
    identity.interfaceName = "NVMe";
    identity.rotationRate = 1;

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    DWORD bufSize = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 1024;
    std::vector<uint8_t> buf(bufSize, 0);
    DWORD returned = 0;

    if (DeviceIoControl(m_hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
                       &query, sizeof(query), buf.data(), bufSize,
                       &returned, nullptr)
        && returned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {

        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf.data());
        if (desc->VendorIdOffset)
            identity.model = ataToWide(std::string(
                reinterpret_cast<const char*>(buf.data() + desc->VendorIdOffset)));
        if (desc->ProductIdOffset) {
            auto p = ataToWide(std::string(
                reinterpret_cast<const char*>(buf.data() + desc->ProductIdOffset)));
            if (!identity.model.empty()) identity.model += L" ";
            identity.model += p;
        }
        if (desc->ProductRevisionOffset)
            identity.firmwareRevision = ataToWide(std::string(
                reinterpret_cast<const char*>(buf.data() + desc->ProductRevisionOffset)));
        if (desc->SerialNumberOffset)
            identity.serialNumber = ataToWide(std::string(
                reinterpret_cast<const char*>(buf.data() + desc->SerialNumberOffset)));
    }

    // Capacity
    {
        GET_LENGTH_INFORMATION lenInfo = {};
        DWORD ret = 0;
        if (DeviceIoControl(m_hDevice, IOCTL_DISK_GET_LENGTH_INFO,
                           nullptr, 0, &lenInfo, sizeof(lenInfo), &ret, nullptr)) {
            identity.capacityBytes = lenInfo.Length.QuadPart;
        }
    }

    identity.sectorSize = 512;
    identity.smartSupported = true;
    identity.smartEnabled = true;

    return !identity.model.empty();
}

// ═══════════════════════════════════════════════════════════════════════
//  Read SMART Attributes
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::readAttributes(std::vector<SmartAttribute>& attributes) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    uint8_t smartLogData[sizeof(NvmeSmartLog)] = {};
    bool ok = false;

    if (m_protocolCommandOk) {
        ok = sendNvmeCommand(
            NVME_ADMIN_GET_LOG_PAGE,
            0xFFFFFFFF,
            0x02 | (((sizeof(NvmeSmartLog) / 4) - 1) << 16),
            0,
            smartLogData, sizeof(NvmeSmartLog), true);
    }

    if (!ok) {
        ok = readSmartLogViaStorageQuery(smartLogData, sizeof(NvmeSmartLog));
    }

    if (!ok) {
        ok = readSmartLogViaPredictFailure(smartLogData, sizeof(NvmeSmartLog));
    }

    if (!ok) return false;

    parseNvmeSmartLog(smartLogData, attributes);
    return !attributes.empty();
}

// ═══════════════════════════════════════════════════════════════════════
//  NVMe doesn't use traditional ATA thresholds
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderNvme::readThresholds(std::vector<SmartAttribute>& /*attributes*/) {
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  NVMe SMART status check — reads Critical Warning field
//  Returns: 0 = good, 1 = warning/critical, -1 = error
// ═══════════════════════════════════════════════════════════════════════

int SmartReaderNvme::checkSmartStatus() {
    if (m_hDevice == INVALID_HANDLE_VALUE) return -1;

    uint8_t smartLogData[sizeof(NvmeSmartLog)] = {};
    bool ok = false;

    // Use the same multi-path approach as readAttributes
    if (m_protocolCommandOk) {
        ok = sendNvmeCommand(
            NVME_ADMIN_GET_LOG_PAGE,
            0xFFFFFFFF,
            0x02 | (((sizeof(NvmeSmartLog) / 4) - 1) << 16),
            0,
            smartLogData, sizeof(NvmeSmartLog), true);
    }

    if (!ok) {
        ok = readSmartLogViaStorageQuery(smartLogData, sizeof(NvmeSmartLog));
    }

    if (!ok) {
        ok = readSmartLogViaPredictFailure(smartLogData, sizeof(NvmeSmartLog));
    }

    if (!ok) return -1;

    const auto* log = reinterpret_cast<const NvmeSmartLog*>(smartLogData);

    // NVMe Critical Warning byte: if any bit is set, the device is in
    // a critical state (available spare below threshold, temperature
    // above threshold, reliability degraded, read-only mode, etc.)
    if (log->critical_warning != 0)
        return 1; // Failing/warning

    return 0; // Good
}
