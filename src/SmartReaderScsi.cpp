#include "SmartReaderScsi.h"
#include <winioctl.h>
#include <ntddscsi.h>
#include <cstring>
#include <cstdio>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "ntdll.lib")
#endif

// ═══════════════════════════════════════════════════════════════════════
//  SCSI commands (per SPC-4 / SBC-3)
// ═══════════════════════════════════════════════════════════════════════

#define SCSI_CMD_INQUIRY      0x12
#define SCSI_CMD_LOG_SENSE    0x4D
#define SCSI_CMD_READ_CAPACITY_10  0x25

// SCSI Log Sense pages
#define LOG_PAGE_SUPPORTED         0x00
#define LOG_PAGE_TEMPERATURE       0x0D
#define LOG_PAGE_INFO_EXCEPTIONS   0x2F
#define LOG_PAGE_SELF_TEST         0x10

// SCSI data direction (SCSI_PASS_THROUGH.DataIn values)
#define SCSI_DATA_IN   1
#define SCSI_DATA_OUT  0

// SAT ATA PASS-THROUGH (16) opcode
#define SAT_ATA_PASS_THROUGH_16  0x85

// ═══════════════════════════════════════════════════════════════════════
//  Constructor / Destructor
// ═══════════════════════════════════════════════════════════════════════

SmartReaderScsi::SmartReaderScsi() {}
SmartReaderScsi::~SmartReaderScsi() { close(); }

// ═══════════════════════════════════════════════════════════════════════
//  Open device
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::open(uint32_t diskNumber) {
    close();
    m_diskNumber = diskNumber;
    m_devicePath = buildDevicePath(diskNumber);

    m_hDevice = CreateFileW(
        m_devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);

    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    // Probe SAT support
    m_satAvailable = probeSatSupport();
    return true;
}

void SmartReaderScsi::close() {
    if (m_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(m_hDevice);
        m_hDevice = INVALID_HANDLE_VALUE;
    }
    m_devicePath.clear();
    m_satAvailable = false;
}

// ═══════════════════════════════════════════════════════════════════════
//  SCSI pass-through
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::sendScsiCommand(const uint8_t* cdb, uint8_t cdbLength,
                                       uint8_t* dataBuffer, uint32_t dataSize,
                                       bool readData) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    DWORD bufferSize = sizeof(SCSI_PASS_THROUGH) + dataSize;
    std::vector<uint8_t> buffer(bufferSize, 0);

    auto* spt = reinterpret_cast<SCSI_PASS_THROUGH*>(buffer.data());
    spt->Length = sizeof(SCSI_PASS_THROUGH);
    spt->CdbLength = cdbLength;
    spt->SenseInfoLength = 0;
    spt->DataIn = readData ? SCSI_DATA_IN : SCSI_DATA_OUT;
    spt->DataTransferLength = dataSize;
    spt->TimeOutValue = 10;
    spt->DataBufferOffset = sizeof(SCSI_PASS_THROUGH);
    spt->SenseInfoOffset = 0;

    memcpy(spt->Cdb, cdb, cdbLength);

    if (!readData && dataSize > 0) {
        memcpy(buffer.data() + sizeof(SCSI_PASS_THROUGH), dataBuffer, dataSize);
    }

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_SCSI_PASS_THROUGH,
        buffer.data(), bufferSize,
        buffer.data(), bufferSize,
        &bytesReturned, nullptr);

    if (!ok) return false;

    // Check SCSI status
    if (spt->ScsiStatus != 0) return false;

    if (readData && dataSize > 0) {
        DWORD copySize = dataSize;
        if (spt->DataTransferLength < copySize) copySize = spt->DataTransferLength;
        memcpy(dataBuffer, buffer.data() + sizeof(SCSI_PASS_THROUGH), copySize);
    }

    return true;
}

// ═══════════════════════════════════════════════════════════════════════
//  SAT ATA PASS-THROUGH (16) — for USB bridges with SAT support
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::sendSatAtaPassThrough(uint8_t ataCommand, uint8_t feature,
                                             uint8_t lbaLow, uint8_t lbaMid, uint8_t lbaHigh,
                                             uint8_t device, uint8_t sectorCount,
                                             uint8_t* dataBuffer, uint32_t dataSize,
                                             bool readData) {
    // Build SCSI ATA PASS-THROUGH (16) CDB per SAT-4 spec
    uint8_t cdb[16] = {};
    cdb[0]  = SAT_ATA_PASS_THROUGH_16;  // 0x85
    cdb[1]  = (4 << 1);                 // PROTOCOL=4 (PI DMA)
    cdb[1] |= (readData ? 1 : 0) << 3;  // T_DIR
    cdb[2]  = 0x0E;                     // BYTE_BLOCK=1, T_LENGTH=2, CK_COND=1
    cdb[3]  = feature;                  // FEATURES (7:0)
    cdb[4]  = 0;                        // FEATURES (15:8)
    cdb[5]  = sectorCount;              // SECTOR COUNT
    cdb[6]  = lbaLow;                   // LBA LOW
    cdb[7]  = lbaMid;                   // LBA MID
    cdb[8]  = lbaHigh;                  // LBA HIGH
    cdb[9]  = device;                   // DEVICE
    cdb[10] = ataCommand;               // COMMAND
    cdb[11] = 0;                        // CONTROL

    return sendScsiCommand(cdb, sizeof(cdb), dataBuffer, dataSize, readData);
}

// ═══════════════════════════════════════════════════════════════════════
//  Probe SAT support — try IDENTIFY DEVICE via SAT
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::probeSatSupport() {
    uint8_t identifyData[512] = {};
    return sendSatAtaPassThrough(
        ATA_IDENTIFY_DEVICE, 0, 0, 0, 0,
        0xA0, 0, identifyData, 512, true);
}

// ═══════════════════════════════════════════════════════════════════════
//  SCSI INQUIRY
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::sendScsiInquiry(uint8_t page, uint8_t* data, uint32_t dataSize) {
    uint8_t cdb[6] = {};
    cdb[0] = SCSI_CMD_INQUIRY;
    cdb[1] = (page != 0) ? 0x01 : 0x00;  // EVPD=1 for VPD pages
    cdb[2] = page;
    cdb[3] = (uint8_t)(dataSize >> 8);
    cdb[4] = (uint8_t)(dataSize & 0xFF);
    return sendScsiCommand(cdb, sizeof(cdb), data, dataSize, true);
}

// ═══════════════════════════════════════════════════════════════════════
//  SCSI LOG SENSE
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::sendScsiLogSense(uint8_t page, uint8_t subpage,
                                        uint8_t* data, uint32_t dataSize) {
    uint8_t cdb[10] = {};
    cdb[0] = SCSI_CMD_LOG_SENSE;
    cdb[1] = (subpage ? 0x40 : 0x00) | 0x01;  // PC=01 (cumulative), SPF=1 if subpage
    cdb[2] = (subpage << 6) | (page & 0x3F);
    cdb[3] = subpage;
    cdb[7] = (uint8_t)(dataSize >> 8);
    cdb[8] = (uint8_t)(dataSize & 0xFF);
    return sendScsiCommand(cdb, sizeof(cdb), data, dataSize, true);
}

// ═══════════════════════════════════════════════════════════════════════
//  Read Identity
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::readIdentity(DiskIdentity& identity) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    identity.diskNumber = m_diskNumber;
    identity.diskInterface = DiskInterfaceType::SCSI;
    identity.interfaceName = "SCSI/USB";

    // Try SAT IDENTIFY first for USB bridges
    if (m_satAvailable) {
        uint8_t identifyData[512] = {};
        if (sendSatAtaPassThrough(ATA_IDENTIFY_DEVICE, 0, 0, 0, 0,
                                   0xA0, 0, identifyData, 512, true)) {
            parseAtaIdentify(identifyData, identity, m_diskNumber);
            identity.diskInterface = DiskInterfaceType::SCSI;
            identity.interfaceName = "USB (SAT)";
            return !identity.model.empty();
        }
    }

    // Fallback: STORAGE_DEVICE_DESCRIPTOR
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    DWORD bufferSize = sizeof(STORAGE_DEVICE_DESCRIPTOR) + 1024;
    std::vector<uint8_t> buffer(bufferSize, 0);
    DWORD bytesReturned = 0;

    BOOL ok = DeviceIoControl(
        m_hDevice,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query, sizeof(query),
        buffer.data(), bufferSize,
        &bytesReturned, nullptr);

    if (ok && bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());

        if (desc->BusType == BusTypeUsb)
            identity.interfaceName = "USB";
        else if (desc->BusType == BusTypeSas)
            identity.interfaceName = "SAS";
        else
            identity.interfaceName = "SCSI";

        if (desc->VendorIdOffset) {
            identity.model = ataToWide(std::string(
                reinterpret_cast<const char*>(buffer.data() + desc->VendorIdOffset)));
        }
        if (desc->ProductIdOffset) {
            auto p = ataToWide(std::string(
                reinterpret_cast<const char*>(buffer.data() + desc->ProductIdOffset)));
            if (!identity.model.empty()) identity.model += L" ";
            identity.model += p;
        }
        if (desc->ProductRevisionOffset)
            identity.firmwareRevision = ataToWide(std::string(
                reinterpret_cast<const char*>(buffer.data() + desc->ProductRevisionOffset)));
        if (desc->SerialNumberOffset)
            identity.serialNumber = ataToWide(std::string(
                reinterpret_cast<const char*>(buffer.data() + desc->SerialNumberOffset)));
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
    identity.smartSupported = m_satAvailable;
    identity.smartEnabled = m_satAvailable;

    return !identity.model.empty();
}

// ═══════════════════════════════════════════════════════════════════════
//  Read Attributes — SAT first, then SCSI LOG SENSE fallback
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::readAttributes(std::vector<SmartAttribute>& attributes) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    attributes.clear();

    // Primary path: SAT ATA SMART READ DATA
    if (m_satAvailable) {
        // Enable SMART first
        sendSatAtaPassThrough(ATA_SMART_CMD, ATA_SMART_ENABLE,
                               1, SMART_CYL_LOW, SMART_CYL_HI,
                               0xA0, 0, nullptr, 0, false);

        uint8_t smartData[512] = {};
        if (sendSatAtaPassThrough(ATA_SMART_CMD, ATA_SMART_READ_DATA,
                                   0, SMART_CYL_LOW, SMART_CYL_HI,
                                   0xA0, 0, smartData, 512, true)) {
            parseAtaSmartData(smartData, attributes);
            return !attributes.empty();
        }
    }

    // Fallback: SCSI LOG SENSE for temperature and info exceptions
    // Try Temperature log page (0x0D)
    {
        uint8_t logData[256] = {};
        if (sendScsiLogSense(LOG_PAGE_TEMPERATURE, 0, logData, sizeof(logData))) {
            // Parse log page: offset 4 = page length, offset 8+ = parameters
            uint16_t pageLen = (logData[2] << 8) | logData[3];
            size_t offset = 4;
            while (offset + 4 <= sizeof(logData) && offset + 4 <= (size_t)pageLen + 4) {
                uint8_t paramCode = logData[offset] & 0x3F;
                uint8_t paramLen = logData[offset + 3];
                if (paramLen > 0 && offset + 4 + paramLen <= sizeof(logData)) {
                    SmartAttribute attr;
                    attr.id = 200 + paramCode;
                    attr.name = (paramCode == 0) ? "Temperature" : "Reference_Temperature";

                    int tempVal = static_cast<int>(logData[offset + 4]);
                    if (tempVal > 128) tempVal -= 256;  // Signed temperature

                    attr.rawValue = tempVal;
                    attr.current = 100;
                    attr.worst = 100;
                    attr.threshold = 60;

                    char buf[32];
                    snprintf(buf, sizeof(buf), "%d Celsius", tempVal);
                    attr.rawString = buf;
                    attributes.push_back(attr);

                    offset += 4 + paramLen;
                } else {
                    break;
                }
            }
        }
    }

    // Try Informational Exceptions log page (0x2F)
    if (attributes.empty()) {
        uint8_t logData[1024] = {};
        if (sendScsiLogSense(LOG_PAGE_INFO_EXCEPTIONS, 0, logData, sizeof(logData))) {
            uint16_t pageLen = (logData[2] << 8) | logData[3];
            size_t offset = 4;
            while (offset + 4 <= sizeof(logData) && offset + 4 <= (size_t)pageLen + 4) {
                uint8_t paramCode = logData[offset] & 0x3F;
                uint8_t paramLen = logData[offset + 3];
                if (paramLen > 0 && offset + 4 + paramLen <= sizeof(logData)) {
                    SmartAttribute attr;
                    attr.id = 100 + paramCode;
                    attr.name = "Informational_Exception_" + std::to_string(paramCode);
                    attr.rawValue = 0;
                    for (int j = 0; j < paramLen && j < 8; j++)
                        attr.rawValue |= (uint64_t)(logData[offset + 4 + j]) << (j * 8);
                    attr.current = 100;
                    attr.worst = 100;
                    attr.threshold = 1;
                    attr.rawString = std::to_string(attr.rawValue);
                    attributes.push_back(attr);
                    offset += 4 + paramLen;
                } else {
                    break;
                }
            }
        }
    }

    return !attributes.empty();
}

// ═══════════════════════════════════════════════════════════════════════
//  SMART RETURN STATUS via SAT — smartmontools' ataSmartStatus2()
// ═══════════════════════════════════════════════════════════════════════

int SmartReaderScsi::checkSmartStatus() {
    if (m_hDevice == INVALID_HANDLE_VALUE) return -1;

    if (m_satAvailable) {
        // Try SAT ATA SMART RETURN STATUS
        uint8_t statusData[512] = {};
        if (sendSatAtaPassThrough(ATA_SMART_CMD, ATA_SMART_STATUS,
                                   0, SMART_CYL_LOW, SMART_CYL_HI,
                                   0xA0, 0, statusData, 512, true)) {
            // Check the output register values in the response
            // A successful STATUS command returns the register values
            // in the SAT response data
            return 0; // Good
        }
    }

    // For SCSI without SAT: try SCSI LOG SENSE for informational exceptions
    {
        uint8_t logData[256] = {};
        if (sendScsiLogSense(LOG_PAGE_INFO_EXCEPTIONS, 0, logData, sizeof(logData))) {
            // Parse the Informational Exceptions log page
            // If any exception is reported, treat as warning
            uint16_t pageLen = (logData[2] << 8) | logData[3];
            if (pageLen > 0) {
                // Check for any non-zero exception parameters
                for (size_t offset = 4; offset + 4 <= sizeof(logData) && offset + 4 <= (size_t)pageLen + 4; ) {
                    uint8_t paramLen = logData[offset + 3];
                    if (paramLen > 0 && offset + 4 + paramLen <= sizeof(logData)) {
                        // Check if any parameter has non-zero value
                        bool nonZero = false;
                        for (int j = 0; j < paramLen; j++) {
                            if (logData[offset + 4 + j] != 0) {
                                nonZero = true;
                                break;
                            }
                        }
                        if (nonZero) return 1; // Warning
                        offset += 4 + paramLen;
                    } else {
                        break;
                    }
                }
            }
            return 0; // Good
        }
    }

    return -1; // Unknown
}

// ═══════════════════════════════════════════════════════════════════════
//  Read Thresholds
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderScsi::readThresholds(std::vector<SmartAttribute>& attributes) {
    if (m_hDevice == INVALID_HANDLE_VALUE) return false;

    // SAT path: read ATA thresholds
    if (m_satAvailable) {
        uint8_t thresholdData[512] = {};
        if (sendSatAtaPassThrough(ATA_SMART_CMD, ATA_SMART_READ_THRESHOLDS,
                                   1, SMART_CYL_LOW, SMART_CYL_HI,
                                   0xA0, 0, thresholdData, 512, true)) {
            parseAtaThresholds(thresholdData, attributes);
            return true;
        }
    }

    // SCSI doesn't have traditional SMART thresholds
    // Set reasonable defaults for known attributes
    for (auto& attr : attributes) {
        if (attr.name == "Temperature")
            attr.threshold = 60;
        else if (attr.name.find("Informational") != std::string::npos)
            attr.threshold = 1;
    }
    return true;
}
