#include "SmartReaderBase.h"
#include "SmartReaderAta.h"
#include "SmartReaderNvme.h"
#include "SmartReaderScsi.h"
#include <winioctl.h>
#include <cstring>
#include <cstdio>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════
//  Parse ATA SMART data — smartmontools-compatible implementation
//  Parses the 512-byte SMART READ DATA structure per SFF-8035i
// ═══════════════════════════════════════════════════════════════════════

void SmartReaderBase::parseAtaSmartData(const uint8_t* data,
                                         std::vector<SmartAttribute>& attributes) {
    attributes.clear();

    const auto* sv = reinterpret_cast<const AtaSmartValues*>(data);

    for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; ++i) {
        const AtaSmartAttribute& raw = sv->vendor_attributes[i];
        if (raw.id == 0) continue;

        SmartAttribute attr;
        attr.id = raw.id;
        attr.name = ataGetSmartAttrName(raw.id);
        attr.current = raw.current;
        attr.worst = raw.worst;
        attr.preFailure = ATTR_FLAG_PREFAILURE(raw.flags);
        attr.online = ATTR_FLAG_ONLINE(raw.flags);

        const auto& def = g_defaultAttrDefs[raw.id];
        attr.rawFormat = def.raw_format;
        attr.rawValue = ataGetAttrRawValue(raw, def);
        attr.rawString = ataFormatAttrRawValue(raw, def);

        // Copy byteorder
        snprintf(attr.byteorder, sizeof(attr.byteorder), "%s",
                 def.byteorder[0] ? def.byteorder : "543210");

        attributes.push_back(attr);
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Parse ATA SMART thresholds
// ═══════════════════════════════════════════════════════════════════════

void SmartReaderBase::parseAtaThresholds(const uint8_t* data,
                                          std::vector<SmartAttribute>& attributes) {
    const auto* th = reinterpret_cast<const AtaSmartThresholds*>(data);

    for (auto& attr : attributes) {
        for (int i = 0; i < NUMBER_ATA_SMART_ATTRIBUTES; ++i) {
            if (th->thres_entries[i].id == attr.id) {
                attr.threshold = th->thres_entries[i].threshold;
                break;
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Parse NVMe SMART / Health Information Log (512 bytes)
//  Based on smartmontools' nvmecmds.cpp + nvmeprint.cpp
// ═══════════════════════════════════════════════════════════════════════

void SmartReaderBase::parseNvmeSmartLog(const uint8_t* data,
                                         std::vector<SmartAttribute>& attributes) {
    attributes.clear();
    const auto* log = reinterpret_cast<const NvmeSmartLog*>(data);

    // ── 1. Critical Warning ────────────────────────────────────────────
    {
        SmartAttribute a;
        a.id = 0;
        a.name = "Critical_Warning";
        a.rawValue = log->critical_warning;
        a.current = (log->critical_warning == 0) ? 100 : 0;
        a.worst = a.current;
        a.threshold = 1;
        a.preFailure = true;

        std::string flags;
        if (log->critical_warning & 0x01) flags += "available_spare_below_threshold ";
        if (log->critical_warning & 0x02) flags += "temperature_above_threshold ";
        if (log->critical_warning & 0x04) flags += "reliability_degraded ";
        if (log->critical_warning & 0x08) flags += "read_only ";
        if (log->critical_warning & 0x10) flags += "volatile_memory_backup_failed ";
        if (log->critical_warning & 0x20) flags += "persistent_memory_read_only ";
        a.rawString = flags.empty() ? "-" : flags;
        attributes.push_back(a);
    }

    // ── 2. Temperature ─────────────────────────────────────────────────
    {
        uint16_t tempK = log->temperature[0] | (static_cast<uint16_t>(log->temperature[1]) << 8);
        double tempC = static_cast<double>(tempK) - 273.15;

        SmartAttribute a;
        a.id = 194;
        a.name = "Temperature";
        a.rawValue = static_cast<uint64_t>(tempK);
        a.current = 100;
        a.worst = 100;
        a.threshold = 0;
        a.preFailure = true;

        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f Celsius", tempC);
        a.rawString = buf;
        attributes.push_back(a);
    }

    // ── 3. Available Spare ─────────────────────────────────────────────
    {
        SmartAttribute a;
        a.id = 1;
        a.name = "Available_Spare";
        a.rawValue = log->avail_spare;
        a.current = log->avail_spare;
        a.worst = log->avail_spare;
        a.threshold = log->spare_thresh;
        a.preFailure = true;

        char buf[32];
        snprintf(buf, sizeof(buf), "%u%%", log->avail_spare);
        a.rawString = buf;
        attributes.push_back(a);
    }

    // ── 4. Available Spare Threshold ───────────────────────────────────
    {
        SmartAttribute a;
        a.id = 2;
        a.name = "Available_Spare_Threshold";
        a.rawValue = log->spare_thresh;
        a.current = 100;
        a.worst = 100;
        a.threshold = 10;
        a.preFailure = true;

        char buf[32];
        snprintf(buf, sizeof(buf), "%u%%", log->spare_thresh);
        a.rawString = buf;
        attributes.push_back(a);
    }

    // ── 5. Percentage Used ─────────────────────────────────────────────
    {
        SmartAttribute a;
        a.id = 202;
        a.name = "Percentage_Used";
        a.rawValue = log->percent_used;
        a.current = (log->percent_used <= 100) ? (100 - log->percent_used) : 0;
        a.worst = a.current;
        a.threshold = 0;
        a.preFailure = true;

        char buf[32];
        snprintf(buf, sizeof(buf), "%u%%", log->percent_used);
        a.rawString = buf;
        attributes.push_back(a);
    }

    // ── 6-17. Data unit and command counters ───────────────────────────
    struct NvmeCounter {
        uint16_t id;
        const char* name;
        const uint8_t* ptr;
        bool preFail;
    };

    NvmeCounter counters[] = {
        { 241, "Data_Units_Read",    log->data_units_read,    false },
        { 242, "Data_Units_Written", log->data_units_written, false },
        { 243, "Host_Read_Commands", log->host_reads,         false },
        { 244, "Host_Write_Commands",log->host_writes,        false },
        { 245, "Controller_Busy_Time",log->ctrl_busy_time,    false },
        { 12,  "Power_Cycles",       log->power_cycles,       false },
        { 9,   "Power_On_Hours",     log->power_on_hours,     false },
        { 174, "Unsafe_Shutdowns",   log->unsafe_shutdowns,   false },
        { 187, "Media_and_Data_Integrity_Errors", log->media_errors, true },
        { 188, "Number_of_Error_Information_Log_Entries", log->num_err_log_entries, true },
    };

    for (auto& c : counters) {
        SmartAttribute a;
        a.id = static_cast<uint8_t>(c.id);
        a.name = c.name;
        a.rawValue = readUint128le(c.ptr);
        a.current = 100;
        a.worst = 100;
        a.threshold = c.preFail ? 1 : 0;
        a.preFailure = c.preFail;

        // Format: For data units, show in human-readable form
        if (c.id == 241 || c.id == 242) {
            // Data units are in 512*1000 = 512000 bytes
            uint64_t bytes = a.rawValue * 512000ULL;
            char buf[64];
            if (bytes >= (1ULL << 40))  // >= 1 TB
                snprintf(buf, sizeof(buf), "%llu [%.2f TB]",
                         (unsigned long long)a.rawValue,
                         (double)bytes / (1ULL << 40));
            else if (bytes >= (1ULL << 30))  // >= 1 GB
                snprintf(buf, sizeof(buf), "%llu [%.2f GB]",
                         (unsigned long long)a.rawValue,
                         (double)bytes / (1ULL << 30));
            else
                snprintf(buf, sizeof(buf), "%llu [%.2f MB]",
                         (unsigned long long)a.rawValue,
                         (double)bytes / (1ULL << 20));
            a.rawString = buf;
        } else if (c.id == 9) {
            // Power-on hours
            a.rawString = std::to_string(a.rawValue) + " h";
        } else {
            a.rawString = std::to_string(a.rawValue);
        }
        attributes.push_back(a);
    }

    // ── 18. Warning Composite Temperature Time ─────────────────────────
    {
        SmartAttribute a;
        a.id = 190;
        a.name = "Warning_Temp_Time";
        a.rawValue = log->warning_temp_time;
        a.current = 100;
        a.worst = 100;
        a.threshold = 0;
        a.preFailure = true;
        a.rawString = std::to_string(log->warning_temp_time) + " min";
        attributes.push_back(a);
    }

    // ── 19. Critical Composite Temperature Time ────────────────────────
    {
        SmartAttribute a;
        a.id = 191;
        a.name = "Critical_Temp_Time";
        a.rawValue = log->critical_comp_time;
        a.current = 100;
        a.worst = 100;
        a.threshold = 0;
        a.preFailure = true;
        a.rawString = std::to_string(log->critical_comp_time) + " min";
        attributes.push_back(a);
    }

    // ── 20-27. Temperature sensors ─────────────────────────────────────
    for (int i = 0; i < 8; i++) {
        uint16_t tempK = log->temp_sensor[i];
        if (tempK == 0) continue;  // Sensor not present

        double tempC = static_cast<double>(tempK) - 273.15;

        SmartAttribute a;
        a.id = 210 + i;
        a.name = std::string("Temperature_Sensor_") + std::to_string(i + 1);
        a.rawValue = tempK;
        a.current = 100;
        a.worst = 100;
        a.threshold = 0;

        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f Celsius", tempC);
        a.rawString = buf;
        attributes.push_back(a);
    }

    // ── Thermal management temperatures ────────────────────────────────
    if (log->thm_temp1_trans_count || log->thm_temp2_trans_count) {
        {
            SmartAttribute a;
            a.id = 220;
            a.name = "Thermal_Mgmt_Temp1_Transitions";
            a.rawValue = log->thm_temp1_trans_count;
            a.current = 100;
            a.worst = 100;
            a.threshold = 0;
            a.rawString = std::to_string(log->thm_temp1_trans_count);
            attributes.push_back(a);
        }
        {
            SmartAttribute a;
            a.id = 221;
            a.name = "Thermal_Mgmt_Temp2_Transitions";
            a.rawValue = log->thm_temp2_trans_count;
            a.current = 100;
            a.worst = 100;
            a.threshold = 0;
            a.rawString = std::to_string(log->thm_temp2_trans_count);
            attributes.push_back(a);
        }
        {
            SmartAttribute a;
            a.id = 222;
            a.name = "Thermal_Mgmt_Temp1_Time";
            a.rawValue = log->thm_temp1_total_time;
            a.current = 100;
            a.worst = 100;
            a.threshold = 0;
            a.rawString = std::to_string(log->thm_temp1_total_time) + " s";
            attributes.push_back(a);
        }
        {
            SmartAttribute a;
            a.id = 223;
            a.name = "Thermal_Mgmt_Temp2_Time";
            a.rawValue = log->thm_temp2_total_time;
            a.current = 100;
            a.worst = 100;
            a.threshold = 0;
            a.rawString = std::to_string(log->thm_temp2_total_time) + " s";
            attributes.push_back(a);
        }
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Parse ATA IDENTIFY DEVICE data
// ═══════════════════════════════════════════════════════════════════════

void SmartReaderBase::parseAtaIdentify(const uint8_t* data, DiskIdentity& identity,
                                        uint32_t diskNumber) {
    identity.diskNumber = diskNumber;
    identity.diskInterface = DiskInterfaceType::ATA;
    identity.interfaceName = "ATA";

    const auto* id = reinterpret_cast<const AtaIdentifyDevice*>(data);

    // Model: bytes 54-93 (words 27-46, byte-swapped)
    identity.model = ataToWide(extractAtaString(id->model, sizeof(id->model)));

    // Serial: bytes 20-39 (words 10-19)
    identity.serialNumber = ataToWide(extractAtaString(id->serial_no, sizeof(id->serial_no)));

    // Firmware: bytes 46-53 (words 23-26)
    identity.firmwareRevision = ataToWide(extractAtaString(id->fw_rev, sizeof(id->fw_rev)));

    // Detect rotation rate (word 217)
    identity.rotationRate = detectRotationRate(id);

    // SMART support check
    identity.smartSupported = isSmartSupported(id);
    identity.smartEnabled = (isSmartEnabled(id) == 1);

    // Capacity calculation (smartmontools' ata_get_size_info logic)
    uint16_t word49 = id->words047_079[49 - 47];
    if (word49 & 0x0200) {  // LBA supported
        uint64_t lba28 = (uint64_t)id->words047_079[61 - 47] << 16 |
                         (uint64_t)id->words047_079[60 - 47];

        uint64_t lba48 = 0;
        if ((id->command_set_2 & 0xc400) == 0x4400) {
            lba48 = (uint64_t)id->words088_255[103 - 88] << 48 |
                    (uint64_t)id->words088_255[102 - 88] << 32 |
                    (uint64_t)id->words088_255[101 - 88] << 16 |
                    (uint64_t)id->words088_255[100 - 88];
        }

        identity.sectorSize = 512;
        uint64_t totalSectors = (lba48 >= lba28 && lba48 > 0) ? lba48 : lba28;
        identity.capacityBytes = totalSectors * 512;
    }
}

// ═══════════════════════════════════════════════════════════════════════
//  Detect rotation rate from IDENTIFY word 217
// ═══════════════════════════════════════════════════════════════════════

int SmartReaderBase::detectRotationRate(const AtaIdentifyDevice* id) {
    uint16_t word217 = id->words088_255[217 - 88];
    if (word217 == 0x0000 || word217 == 0xffff)
        return 0;  // Not reported
    else if (word217 == 0x0001)
        return 1;  // SSD (non-rotating)
    else if (word217 > 0x0400)
        return word217;  // HDD RPM
    else
        return -(int)word217;  // Unknown value
}

// ═══════════════════════════════════════════════════════════════════════
//  SMART support check — smartmontools' ataSmartSupport()
// ═══════════════════════════════════════════════════════════════════════

bool SmartReaderBase::isSmartSupported(const AtaIdentifyDevice* drive) {
    uint16_t word82 = drive->command_set_1;
    uint16_t word83 = drive->command_set_2;

    // Check if words 82/83 contain valid info
    if ((word83 >> 14) == 0x01)
        return (word82 & 0x0001) != 0;

    // Can't tell — assume supported for further probing
    return true;
}

int SmartReaderBase::isSmartEnabled(const AtaIdentifyDevice* drive) {
    uint16_t word85 = drive->cfs_enable_1;
    uint16_t word87 = drive->csf_default;

    if ((word87 >> 14) == 0x01)
        return (word85 & 0x0001) ? 1 : 0;

    return -1;  // Can't tell
}

// ═══════════════════════════════════════════════════════════════════════
//  Factory: detect bus type and create appropriate reader
//  Smartmontools-style auto-detection
// ═══════════════════════════════════════════════════════════════════════

std::unique_ptr<SmartReaderBase> createSmartReader(uint32_t diskNumber) {
    wchar_t path[64];
    swprintf(path, 64, L"\\\\.\\PhysicalDrive%u", diskNumber);

    // First, detect bus type via STORAGE_PROPERTY_QUERY (non-intrusive)
    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;

    std::vector<uint8_t> propBuf(sizeof(STORAGE_DEVICE_DESCRIPTOR) + 512, 0);
    DWORD bytesReturned = 0;

    BOOL propOk = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                                   &query, sizeof(query),
                                   propBuf.data(), static_cast<DWORD>(propBuf.size()),
                                   &bytesReturned, nullptr);
    CloseHandle(h);

    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    if (propOk && bytesReturned >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(propBuf.data());
        busType = desc->BusType;
    }

    // NVMe (BusTypeNvme = 17 in Windows 10+ SDK)
    if (busType == BusTypeNvme) {
        auto nvme = std::make_unique<SmartReaderNvme>();
        if (nvme->open(diskNumber)) {
            DiskIdentity id;
            if (nvme->readIdentity(id) && !id.model.empty())
                return nvme;
            nvme->close();
        }
    }

    // ATA/SATA/RAID/USB — try ATA reader first
    if (busType == BusTypeAta || busType == BusTypeSata ||
        busType == BusTypeSas || busType == BusTypeRAID ||
        busType == BusTypeUsb || busType == BusTypeUnknown) {
        auto ata = std::make_unique<SmartReaderAta>();
        if (ata->open(diskNumber)) {
            DiskIdentity id;
            if (ata->readIdentity(id) && !id.model.empty())
                return ata;
            ata->close();
        }
    }

    // Fallback: SCSI (USB bridges without SAT, etc.)
    auto scsi = std::make_unique<SmartReaderScsi>();
    if (scsi->open(diskNumber)) {
        DiskIdentity id;
        if (scsi->readIdentity(id) && !id.model.empty())
            return scsi;
        scsi->close();
    }

    // Last resort: try ATA again
    auto ata = std::make_unique<SmartReaderAta>();
    if (ata->open(diskNumber)) {
        DiskIdentity id;
        if (ata->readIdentity(id) && !id.model.empty())
            return ata;
        ata->close();
    }

    return nullptr;
}
