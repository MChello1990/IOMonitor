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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <chrono>

// ═══════════════════════════════════════════════════════════════════════
//  Data model — mirrors smartmontools' atacmds.h / nvmecmds.h design
// ═══════════════════════════════════════════════════════════════════════

// ── Enums ────────────────────────────────────────────────────────────

enum class DiskInterfaceType {
    Unknown = 0,
    ATA,        // SATA / PATA
    NVMe,
    SCSI,       // USB-bridged, SAS, etc.
};

enum class SmartStatus {
    OK      = 0,
    Warning = 1,
    Failed  = 2,
    Unknown = 3,
};

// ── ATA SMART structures per SFF-8035i / ATA-ATAPI spec ──────────────
// Mirrors smartmontools atacmds.h structures exactly

// Maximum SMART attributes per ATA spec
#define NUMBER_ATA_SMART_ATTRIBUTES 30
#define MAX_ATTRIBUTE_NUM 256

// Raw value print formats — exact match of smartmontools' ata_attr_raw_format
enum class AttrRawFormat : uint8_t {
    RAWFMT_DEFAULT = 0,
    RAWFMT_RAW8,
    RAWFMT_RAW16,
    RAWFMT_RAW48,
    RAWFMT_HEX48,
    RAWFMT_RAW56,
    RAWFMT_HEX56,
    RAWFMT_RAW64,
    RAWFMT_HEX64,
    RAWFMT_RAW16_OPT_RAW16,
    RAWFMT_RAW16_OPT_AVG16,
    RAWFMT_RAW24_OPT_RAW8,
    RAWFMT_RAW24_DIV_RAW24,
    RAWFMT_RAW24_DIV_RAW32,
    RAWFMT_SEC2HOUR,
    RAWFMT_MIN2HOUR,
    RAWFMT_HALFMIN2HOUR,
    RAWFMT_MSEC24_HOUR32,
    RAWFMT_TEMPMINMAX,
    RAWFMT_TEMP10X,
};

// Attribute flags — ATTRFLAG_*
enum AttrFlags : uint32_t {
    ATTRFLAG_INCREASING  = 0x01,  // Value not reset
    ATTRFLAG_NO_NORMVAL  = 0x02,  // Normalized value not valid
    ATTRFLAG_NO_WORSTVAL = 0x04,  // Worst value not valid
    ATTRFLAG_HDD_ONLY    = 0x08,  // DEFAULT setting for HDD only
    ATTRFLAG_SSD_ONLY    = 0x10,  // DEFAULT setting for SSD only
};

// ── ATA SMART attribute struct (12 bytes, packed) ────────────────────
// Per SFF-8035i Revision 2, Table 42
#pragma pack(push, 1)
struct AtaSmartAttribute {
    uint8_t     id;
    uint16_t    flags;
    uint8_t     current;
    uint8_t     worst;
    uint8_t     raw[6];
    uint8_t     reserv;
};
#pragma pack(pop)
static_assert(sizeof(AtaSmartAttribute) == 12, "AtaSmartAttribute must be 12 bytes");

// Attribute flags (from SFF-8035i)
#define ATTR_FLAG_PREFAILURE(x)   ((x) & 0x01)
#define ATTR_FLAG_ONLINE(x)       ((x) & 0x02)
#define ATTR_FLAG_PERFORMANCE(x)  ((x) & 0x04)
#define ATTR_FLAG_ERRORRATE(x)    ((x) & 0x08)
#define ATTR_FLAG_EVENTCOUNT(x)   ((x) & 0x10)
#define ATTR_FLAG_SELFPRESERVING(x) ((x) & 0x20)

// ── ATA SMART data structure (512 bytes) ─────────────────────────────
#pragma pack(push, 1)
struct AtaSmartValues {
    uint16_t          revnumber;
    AtaSmartAttribute vendor_attributes[NUMBER_ATA_SMART_ATTRIBUTES];
    uint8_t           offline_data_collection_status;
    uint8_t           self_test_exec_status;
    uint16_t          total_time_to_complete_off_line;
    uint8_t           vendor_specific_366;
    uint8_t           offline_data_collection_capability;
    uint16_t          smart_capability;
    uint8_t           errorlog_capability;
    uint8_t           vendor_specific_371;
    uint8_t           short_test_completion_time;
    uint8_t           extend_test_completion_time_b;
    uint8_t           conveyance_test_completion_time;
    uint16_t          extend_test_completion_time_w;
    uint8_t           reserved_377_385[9];
    uint8_t           vendor_specific_386_510[125];
    uint8_t           chksum;
};
#pragma pack(pop)
static_assert(sizeof(AtaSmartValues) == 512, "AtaSmartValues must be 512 bytes");

// ── ATA SMART threshold entry (12 bytes) ─────────────────────────────
#pragma pack(push, 1)
struct AtaSmartThresholdEntry {
    uint8_t id;
    uint8_t threshold;
    uint8_t reserved[10];
};
#pragma pack(pop)
static_assert(sizeof(AtaSmartThresholdEntry) == 12, "AtaSmartThresholdEntry must be 12 bytes");

// ── ATA SMART thresholds data (512 bytes) ────────────────────────────
#pragma pack(push, 1)
struct AtaSmartThresholds {
    uint16_t                 revnumber;
    AtaSmartThresholdEntry   thres_entries[NUMBER_ATA_SMART_ATTRIBUTES];
    uint8_t                  reserved[149];
    uint8_t                  chksum;
};
#pragma pack(pop)
static_assert(sizeof(AtaSmartThresholds) == 512, "AtaSmartThresholds must be 512 bytes");

// ── ATA IDENTIFY DEVICE data (512 bytes) ─────────────────────────────
#pragma pack(push, 1)
struct AtaIdentifyDevice {
    uint16_t words000_009[10];
    uint8_t  serial_no[20];
    uint16_t words020_022[3];
    uint8_t  fw_rev[8];
    uint8_t  model[40];
    uint16_t words047_079[33];
    uint16_t major_rev_num;
    uint16_t minor_rev_num;
    uint16_t command_set_1;
    uint16_t command_set_2;
    uint16_t command_set_extension;
    uint16_t cfs_enable_1;
    uint16_t word086;
    uint16_t csf_default;
    uint16_t words088_255[168];
};
#pragma pack(pop)
static_assert(sizeof(AtaIdentifyDevice) == 512, "AtaIdentifyDevice must be 512 bytes");

// ── NVMe SMART / Health Information Log (512 bytes) ──────────────────
// Per NVMe Base Spec 2.0a, Figure 205
#pragma pack(push, 1)
struct NvmeSmartLog {
    uint8_t  critical_warning;
    uint8_t  temperature[2];        // Composite Temperature in Kelvin
    uint8_t  avail_spare;
    uint8_t  spare_thresh;
    uint8_t  percent_used;
    uint8_t  rsvd6[26];
    uint8_t  data_units_read[16];   // In units of 512*1000 bytes
    uint8_t  data_units_written[16];
    uint8_t  host_reads[16];
    uint8_t  host_writes[16];
    uint8_t  ctrl_busy_time[16];
    uint8_t  power_cycles[16];
    uint8_t  power_on_hours[16];
    uint8_t  unsafe_shutdowns[16];
    uint8_t  media_errors[16];
    uint8_t  num_err_log_entries[16];
    uint32_t warning_temp_time;
    uint32_t critical_comp_time;
    uint16_t temp_sensor[8];
    uint32_t thm_temp1_trans_count;
    uint32_t thm_temp2_trans_count;
    uint32_t thm_temp1_total_time;
    uint32_t thm_temp2_total_time;
    uint8_t  rsvd232[280];
};
#pragma pack(pop)
static_assert(sizeof(NvmeSmartLog) == 512, "NvmeSmartLog must be 512 bytes");

// ── NVMe Identify Controller (4096 bytes) ────────────────────────────
// Note: Only basic fields needed for identity; full struct is 4096 bytes per NVMe spec.
// Using a simplified representation for this project's needs.
#pragma pack(push, 1)
struct NvmeIdCtrl {
    uint16_t vid;
    uint16_t ssvid;
    char     sn[20];
    char     mn[40];
    char     fr[8];
    uint8_t  rab;
    uint8_t  ieee[3];
    uint8_t  cmic;
    uint8_t  mdts;
    uint16_t cntlid;
    uint32_t ver;
    uint32_t rtd3r;
    uint32_t rtd3e;
    uint32_t oaes;
    uint32_t ctratt;
    uint8_t  rsvd100[156];
    uint16_t oacs;
    uint8_t  acl;
    uint8_t  aerl;
    uint8_t  frmw;
    uint8_t  lpa;
    uint8_t  elpe;
    uint8_t  npss;
    uint8_t  avscc;
    uint8_t  apsta;
    uint16_t wctemp;
    uint16_t cctemp;
    uint16_t mtfa;
    uint32_t hmpre;
    uint32_t hmmin;
    uint8_t  tnvmcap[16];
    uint8_t  unvmcap[16];
    uint32_t rpmbs;
    uint16_t edstt;
    uint8_t  dsto;
    uint8_t  fwug;
    uint16_t kas;
    uint16_t hctma;
    uint16_t mntmt;
    uint16_t mxtmt;
    uint32_t sanicap;
    uint8_t  rsvd332[180];
    uint8_t  sqes;
    uint8_t  cqes;
    uint16_t maxcmd;
    uint32_t nn;
    uint16_t oncs;
    uint16_t fuses;
    uint8_t  fna;
    uint8_t  vwc;
    uint16_t awun;
    uint16_t awupf;
    uint8_t  nvscc;
    uint8_t  rsvd531;
    uint16_t acwu;
    uint8_t  rsvd534[2];
    uint32_t sgls;
    uint8_t  rsvd540[3556];
};
#pragma pack(pop)
static_assert(sizeof(NvmeIdCtrl) == 4096, "NvmeIdCtrl must be 4096 bytes");

// ── Single SMART attribute (GUI-facing) ──────────────────────────────
struct SmartAttribute {
    uint8_t     id            = 0;
    std::string name;
    uint8_t     current       = 0;
    uint8_t     worst         = 0;
    uint64_t    rawValue      = 0;
    uint8_t     threshold     = 0;
    bool        preFailure    = false;
    bool        online        = false;
    std::string rawString;
    AttrRawFormat rawFormat   = AttrRawFormat::RAWFMT_DEFAULT;
    char        byteorder[9]  = {}; // Smartmontools-style byte order
};

// ── Disk identity ────────────────────────────────────────────────────
struct DiskIdentity {
    uint32_t    diskNumber       = 0;
    std::wstring model;
    std::wstring serialNumber;
    std::wstring firmwareRevision;
    uint64_t    capacityBytes    = 0;
    uint32_t    sectorSize       = 0;
    DiskInterfaceType diskInterface = DiskInterfaceType::Unknown;
    bool        smartSupported   = false;
    bool        smartEnabled     = false;
    std::string interfaceName;
    int         rotationRate     = 0;   // 0=unknown, 1=SSD, >1=HDD rpm
};

// ── Temperature history ──────────────────────────────────────────────
struct TempPoint {
    std::chrono::steady_clock::time_point timestamp;
    double celsius = 0.0;
};

// ── Complete SMART snapshot ──────────────────────────────────────────
struct SmartDataSnapshot {
    DiskIdentity identity;
    std::vector<SmartAttribute> attributes;

    SmartStatus status          = SmartStatus::Unknown;
    double      healthPercent   = 0.0;

    double      temperatureCelsius    = 0.0;
    double      maxSessionTemp        = 0.0;

    uint64_t    totalLbasRead     = 0;
    uint64_t    totalLbasWritten  = 0;
    uint64_t    totalBytesRead    = 0;
    uint64_t    totalBytesWritten = 0;

    uint64_t    powerOnHours       = 0;
    uint64_t    sessionPowerOnHours = 0;

    int64_t     wearLevelingCount   = -1;
    int64_t     remainingLifePercent = -1;

    double      readRateMBps  = 0.0;
    double      writeRateMBps = 0.0;

    // SMART RETURN STATUS result — smartmontools' ataSmartStatus2()
    // 0 = good, 1 = threshold exceeded (failing), -1 = not checked/error
    int         smartReturnStatus = -1;

    bool        dataValid       = false;
    std::string errorMessage;
    std::wstring permissionHint;

    std::chrono::steady_clock::time_point sampleTime;
};

// ── Attribute definition entry ───────────────────────────────────────
// Maps smartmontools' ata_vendor_attr_defs::entry
struct AttrDefEntry {
    std::string   name;
    AttrRawFormat raw_format = AttrRawFormat::RAWFMT_DEFAULT;
    uint32_t      flags      = 0;    // ATTRFLAG_*
    char          byteorder[9] = {};

    bool hasName() const { return !name.empty(); }
};

// ── Health weight constants ──────────────────────────────────────────
namespace SmartHealthWeights {
    constexpr double REALLOCATED_SECTOR_COUNT    = 0.25;
    constexpr double CURRENT_PENDING_SECTOR       = 0.20;
    constexpr double OFFLINE_UNCORRECTABLE        = 0.20;
    constexpr double REALLOCATED_EVENT_COUNT      = 0.05;
    constexpr double SPIN_RETRY_COUNT             = 0.05;
    constexpr double REPORTED_UNCORRECTABLE       = 0.05;
    constexpr double COMMAND_TIMEOUT              = 0.05;
    constexpr double END_TO_END_ERROR             = 0.05;
    constexpr double ULTRA_DMA_CRC_ERROR          = 0.05;
    constexpr double G_SENSE_ERROR_RATE           = 0.05;
    constexpr double WEAR_LEVELING_COUNT          = 0.15;
    constexpr double SSD_REMAINING_LIFE           = 0.20;
    constexpr double ERASE_FAIL_COUNT             = 0.10;
    constexpr double PROGRAM_FAIL_COUNT           = 0.10;
    constexpr double UNEXPECTED_POWER_LOSS        = 0.05;
    constexpr double NVME_MEDIA_ERRORS            = 0.20;
    constexpr double NVME_CRITICAL_WARNING        = 0.30;
    constexpr double NVME_PERCENTAGE_USED         = 0.20;
    constexpr double NVME_TEMPERATURE             = 0.10;
    constexpr double NVME_UNSAFE_SHUTDOWNS        = 0.10;
}

namespace HealthGrades {
    constexpr double EXCELLENT = 90.0;
    constexpr double GOOD      = 70.0;
    constexpr double WARNING   = 50.0;
}

// ── Default attribute definitions — exact replica of smartmontools DEFAULT drivedb.h entry ──
extern const AttrDefEntry g_defaultAttrDefs[MAX_ATTRIBUTE_NUM];

// ── Attribute state enum (smartmontools ata_attr_state) ──────────────
enum class AttrState {
    NON_EXISTING,
    NO_NORMVAL,
    NO_THRESHOLD,
    OK,
    FAILED_PAST,
    FAILED_NOW
};

// ── Utility functions ────────────────────────────────────────────────
uint64_t ataGetAttrRawValue(const AtaSmartAttribute& attr, const AttrDefEntry& def);
std::string ataFormatAttrRawValue(const AtaSmartAttribute& attr, const AttrDefEntry& def);
std::string ataGetSmartAttrName(uint8_t id, int rpm = 0);
AttrState ataGetAttrState(const AtaSmartAttribute& attr, int attridx,
                          const AtaSmartThresholdEntry* thresholds,
                          uint8_t* threshval = nullptr);
int ataFindAttrIndex(uint8_t id, const AtaSmartValues& smartval);
uint8_t ataReturnTemperatureValue(const AtaSmartValues* data);
unsigned char checksum(const void* data);
std::string extractAtaString(const uint8_t* buf, size_t len);
std::wstring ataToWide(const std::string& s);
uint64_t readUint128le(const uint8_t* p);
