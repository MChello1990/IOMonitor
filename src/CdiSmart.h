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
//  CdiSmart — S.M.A.R.T. acquisition layer
//
//  This module is a direct port of the S.M.A.R.T. related parts of
//  CrystalDiskInfo <https://github.com/hiyohiyo/CrystalDiskInfo>
//  (Copyright (c) hiyohiyo, MIT License) together with the NVMe SMART
//  interpreter from <https://github.com/ebangin127/> (MIT License),
//  which CrystalDiskInfo bundles as NVMeInterpreter.
//
//  The following CrystalDiskInfo components were ported:
//    * AtaSmart.h / AtaSmart.cpp ....... CAtaSmart::GetSmartAttribute*
//                                        CAtaSmart::GetSmartThreshold*
//                                        CAtaSmart::ControlSmartStatus*
//                                        CAtaSmart::DoIdentifyDevice*
//                                        CAtaSmart::FillSmartData()
//                                        CAtaSmart::FillSmartThreshold()
//                                        CAtaSmart::CheckDiskStatus()
//                                        CAtaSmart::MeasuredTimeUnit()
//                                        CAtaSmart::GetPowerOnHours()
//    * NVMeInterpreter.h / .cpp ........ NVMeSmartToATASmart()
//                                        NVMeCompositeTemperatureSmartToATASmart()
//                                        NVMeTemperatureSensorSmartToATASmart()
//                                        NVMeThermalManagementTemperatureSmartToATASmart()
//    * SPTIUtil.h ...................... SCSI pass-through structures
//    * StorageQuery.h .................. Storage Protocol Specific Query
//
//  Only the mainstream Windows 10 / Windows 11 code paths are ported:
//    - IOCTL_ATA_PASS_THROUGH            (native SATA/AHCI + RAID physical drive)
//    - DFP_RECEIVE_DRIVE_DATA            (legacy ATA/SMART IOCTL fallback)
//    - IOCTL_IDE_PASS_THROUGH            (legacy Windows 2000+ path)
//    - SCSI ATA PASS-THROUGH (12)        (USB bridges / SAT, targets 0xA0 / 0xB0)
//    - IOCTL_SCSI_MINIPORT               (legacy SMART_RCV_DRIVE_DATA)
//    - IOCTL_STORAGE_QUERY_PROPERTY      (NVMe via StorageAdapterProtocolSpecificProperty)
//  Vendor specific NVMe tunnels (Samsung / Intel RST / JMicron / ASMedia /
//  Realtek / MegaRAID / CSMI / AMD-RC2 / Silicon Image) are intentionally
//  not ported; they never apply to a stock Windows 10 / 11 installation.
// ═══════════════════════════════════════════════════════════════════════

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <string>
#include <vector>
#include <cstdint>

// ── FILE_DEVICE_SCSI ──────────────────────────────────────────────────
//
// Declared by the Windows SDK's ntddscsi.h but absent from mingw-w64's copy,
// so it is pinned here.  The value is fixed by the Windows device-type table
// and has never changed.
#ifndef FILE_DEVICE_SCSI
#define FILE_DEVICE_SCSI 0x001B
#endif

// ── Neutralise the Windows macros that clash with the constants below ──
//
// winioctl.h and ntddscsi.h define these as object-like macros, so without
// the #undef the constexpr declarations further down would expand to
// nonsense such as `constexpr BYTE 0xB0 = 0xB0;`.  CrystalDiskInfo avoids
// the clash by declaring its copies as an enum *before* including the
// Windows headers; the values are identical to the SDK's, so nothing is
// lost by unbinding the macro names here.
//
// Only the names this header re-declares are undone — IOCTL_* and the
// struct declarations Windows contributes are untouched.
#ifdef IDENTIFY_BUFFER_SIZE
#undef IDENTIFY_BUFFER_SIZE
#endif
#ifdef READ_ATTRIBUTE_BUFFER_SIZE
#undef READ_ATTRIBUTE_BUFFER_SIZE
#endif
#ifdef READ_THRESHOLD_BUFFER_SIZE
#undef READ_THRESHOLD_BUFFER_SIZE
#endif
#ifdef SMART_CMD
#undef SMART_CMD
#endif
#ifdef READ_ATTRIBUTES
#undef READ_ATTRIBUTES
#endif
#ifdef READ_THRESHOLDS
#undef READ_THRESHOLDS
#endif
#ifdef ENABLE_SMART
#undef ENABLE_SMART
#endif
#ifdef DISABLE_SMART
#undef DISABLE_SMART
#endif
#ifdef SMART_CYL_LOW
#undef SMART_CYL_LOW
#endif
#ifdef SMART_CYL_HI
#undef SMART_CYL_HI
#endif
#ifdef ID_CMD
#undef ID_CMD
#endif
#ifdef SMART_READ_LOG
#undef SMART_READ_LOG
#endif
#ifdef ATA_FLAGS_DRDY_REQUIRED
#undef ATA_FLAGS_DRDY_REQUIRED
#endif
#ifdef ATA_FLAGS_DATA_IN
#undef ATA_FLAGS_DATA_IN
#endif
#ifdef ATA_FLAGS_DATA_OUT
#undef ATA_FLAGS_DATA_OUT
#endif
#ifdef ATA_FLAGS_48BIT_COMMAND
#undef ATA_FLAGS_48BIT_COMMAND
#endif

namespace cdi {

// ── CrystalDiskInfo constants ─────────────────────────────────────────
// MAX_DISK / MAX_ATTRIBUTE / MAX_SEARCH_PHYSICAL_DRIVE mirror CAtaSmart
constexpr int MAX_ATTRIBUTE              = 30;
constexpr int MAX_DISK                   = 80;
constexpr int MAX_SEARCH_PHYSICAL_DRIVE  = 56;

constexpr int IDENTIFY_BUFFER_SIZE         = 512;
constexpr int READ_ATTRIBUTE_BUFFER_SIZE   = 512;
constexpr int READ_THRESHOLD_BUFFER_SIZE   = 512;

// ATA command / register values — CrystalDiskInfo AtaSmart.h
constexpr BYTE SMART_CMD        = 0xB0;
constexpr BYTE READ_ATTRIBUTES  = 0xD0;
constexpr BYTE READ_THRESHOLDS  = 0xD1;
constexpr BYTE ENABLE_SMART     = 0xD8;
constexpr BYTE DISABLE_SMART    = 0xD9;
constexpr BYTE SMART_STATUS     = 0xDA;
constexpr BYTE SMART_CYL_LOW    = 0x4F;
constexpr BYTE SMART_CYL_HI     = 0xC2;
constexpr BYTE ID_CMD           = 0xEC;
constexpr BYTE SMART_READ_LOG   = 0xD5;
constexpr BYTE READ_LOG_EXT     = 0x2F;

// ATA_PASS_THROUGH_EX AtaFlags — CrystalDiskInfo AtaSmart.h
constexpr USHORT ATA_FLAGS_DRDY_REQUIRED  = 0x01;
constexpr USHORT ATA_FLAGS_DATA_IN        = 0x02;
constexpr USHORT ATA_FLAGS_DATA_OUT       = 0x04;
constexpr USHORT ATA_FLAGS_48BIT_COMMAND  = 0x08;

// Legacy IOCTL codes — CrystalDiskInfo CAtaSmart::IO_CONTROL_CODE
constexpr DWORD DFP_SEND_DRIVE_COMMAND  = 0x0007C084;
constexpr DWORD DFP_RECEIVE_DRIVE_DATA  = 0x0007C088;
constexpr DWORD IOCTL_SCSI_MINIPORT_CDI  = 0x0004D008;
constexpr DWORD IOCTL_IDE_PASS_THROUGH_CDI = 0x0004D028;

// SCSI miniport SMART control codes — CrystalDiskInfo SPTIUtil.h
constexpr DWORD IOCTL_SCSI_MINIPORT_IDENTIFY              = ((FILE_DEVICE_SCSI << 16) + 0x0501);
constexpr DWORD IOCTL_SCSI_MINIPORT_READ_SMART_ATTRIBS    = ((FILE_DEVICE_SCSI << 16) + 0x0502);
constexpr DWORD IOCTL_SCSI_MINIPORT_READ_SMART_THRESHOLDS = ((FILE_DEVICE_SCSI << 16) + 0x0503);
constexpr DWORD IOCTL_SCSI_MINIPORT_ENABLE_SMART          = ((FILE_DEVICE_SCSI << 16) + 0x0504);
constexpr DWORD IOCTL_SCSI_MINIPORT_DISABLE_SMART         = ((FILE_DEVICE_SCSI << 16) + 0x0505);

// ── CrystalDiskInfo SMART structures ─────────────────────────────────

#pragma pack(push, 1)
struct SMART_ATTRIBUTE {
    BYTE Id;
    WORD StatusFlags;
    BYTE CurrentValue;
    BYTE WorstValue;
    BYTE RawValue[6];
    BYTE Reserved;
};
#pragma pack(pop)
static_assert(sizeof(SMART_ATTRIBUTE) == 12, "SMART_ATTRIBUTE must be 12 bytes");

using SMART_ATTRIBUTE_LIST = SMART_ATTRIBUTE[MAX_ATTRIBUTE];

#pragma pack(push, 1)
struct SMART_THRESHOLD {
    BYTE Id;
    BYTE ThresholdValue;
    BYTE Reserved[10];
};
#pragma pack(pop)
static_assert(sizeof(SMART_THRESHOLD) == 12, "SMART_THRESHOLD must be 12 bytes");

// ATA IDENTIFY DEVICE layout — CrystalDiskInfo AtaSmart.h (word index comments kept)
#pragma pack(push, 1)
struct ATA_IDENTIFY_DEVICE {
    WORD      GeneralConfiguration;             //   0
    WORD      LogicalCylinders;                 //   1  Obsolete
    WORD      SpecificConfiguration;            //   2
    WORD      LogicalHeads;                     //   3  Obsolete
    WORD      Retired1[2];                      //   4- 5
    WORD      LogicalSectors;                   //   6  Obsolete
    DWORD     ReservedForCompactFlash;          //   7- 8
    WORD      Retired2;                         //   9
    CHAR      SerialNumber[20];                 //  10-19
    WORD      Retired3;                         //  20
    WORD      BufferSize;                       //  21  Obsolete
    WORD      Obsolute4;                        //  22
    CHAR      FirmwareRev[8];                   //  23-26
    CHAR      Model[40];                        //  27-46
    WORD      MaxNumPerInterupt;                //  47
    WORD      Reserved1;                        //  48
    WORD      Capabilities1;                    //  49
    WORD      Capabilities2;                    //  50
    DWORD     Obsolute5;                        //  51-52
    WORD      Field88and7064;                   //  53
    WORD      Obsolute6[5];                     //  54-58
    WORD      MultSectorStuff;                  //  59
    DWORD     TotalAddressableSectors;           //  60-61
    WORD      Obsolute7;                        //  62
    WORD      MultiWordDma;                     //  63
    WORD      PioMode;                          //  64
    WORD      MinMultiwordDmaCycleTime;         //  65
    WORD      RecommendedMultiwordDmaCycleTime; //  66
    WORD      MinPioCycleTimewoFlowCtrl;        //  67
    WORD      MinPioCycleTimeWithFlowCtrl;      //  68
    WORD      Reserved2[6];                     //  69-74
    WORD      QueueDepth;                       //  75
    WORD      SerialAtaCapabilities;            //  76
    WORD      SerialAtaAdditionalCapabilities;  //  77
    WORD      SerialAtaFeaturesSupported;       //  78
    WORD      SerialAtaFeaturesEnabled;         //  79
    WORD      MajorVersion;                     //  80
    WORD      MinorVersion;                     //  81
    WORD      CommandSetSupported1;             //  82
    WORD      CommandSetSupported2;             //  83
    WORD      CommandSetSupported3;             //  84
    WORD      CommandSetEnabled1;               //  85
    WORD      CommandSetEnabled2;               //  86
    WORD      CommandSetDefault;                //  87
    WORD      UltraDmaMode;                     //  88
    WORD      TimeReqForSecurityErase;          //  89
    WORD      TimeReqForEnhancedSecure;         //  90
    WORD      CurrentPowerManagement;           //  91
    WORD      MasterPasswordRevision;           //  92
    WORD      HardwareResetResult;              //  93
    WORD      AcoustricManagement;              //  94
    WORD      StreamMinRequestSize;             //  95
    WORD      StreamingTimeDma;                 //  96
    WORD      StreamingAccessLatency;           //  97
    DWORD     StreamingPerformance;             //  98-99
    ULONGLONG MaxUserLba;                       // 100-103
    WORD      StremingTimePio;                  // 104
    WORD      Reserved3;                        // 105
    WORD      SectorSize;                       // 106
    WORD      InterSeekDelay;                   // 107
    WORD      IeeeOui;                          // 108
    WORD      UniqueId3;                        // 109
    WORD      UniqueId2;                        // 110
    WORD      UniqueId1;                        // 111
    WORD      Reserved4[4];                     // 112-115
    WORD      Reserved5;                        // 116
    DWORD     WordsPerLogicalSector;            // 117-118
    WORD      Reserved6[8];                     // 119-126
    WORD      RemovableMediaStatus;             // 127
    WORD      SecurityStatus;                   // 128
    WORD      VendorSpecific[31];               // 129-159
    WORD      CfaPowerMode1;                    // 160
    WORD      ReservedForCompactFlashAssociation[7]; // 161-167
    WORD      DeviceNominalFormFactor;          // 168
    WORD      DataSetManagement;                // 169
    WORD      AdditionalProductIdentifier[4];   // 170-173
    WORD      Reserved7[2];                     // 174-175
    CHAR      CurrentMediaSerialNo[60];         // 176-205
    WORD      SctCommandTransport;              // 206
    WORD      ReservedForCeAta1[2];             // 207-208
    WORD      AlignmentOfLogicalBlocks;         // 209
    DWORD     WriteReadVerifySectorCountMode3;  // 210-211
    DWORD     WriteReadVerifySectorCountMode2;  // 212-213
    WORD      NvCacheCapabilities;              // 214
    DWORD     NvCacheSizeLogicalBlocks;         // 215-216
    WORD      NominalMediaRotationRate;         // 217
    WORD      Reserved8;                        // 218
    WORD      NvCacheOptions1;                  // 219
    WORD      NvCacheOptions2;                  // 220
    WORD      Reserved9;                        // 221
    WORD      TransportMajorVersionNumber;      // 222
    WORD      TransportMinorVersionNumber;      // 223
    WORD      ReservedForCeAta2[10];            // 224-233
    WORD      MinimumBlocksPerDownloadMicrocode;// 234
    WORD      MaximumBlocksPerDownloadMicrocode;// 235
    WORD      Reserved10[19];                   // 236-254
    WORD      IntegrityWord;                    // 255
};
#pragma pack(pop)
static_assert(sizeof(ATA_IDENTIFY_DEVICE) == 512, "ATA_IDENTIFY_DEVICE must be 512 bytes");

// NVMe Identify Controller (subset used for identity + threshold detection)
#pragma pack(push, 1)
struct NVME_IDENTIFY_DEVICE {
    WORD  PCIeSubSysVID;
    WORD  PCIeVID;
    CHAR  SerialNumber[20];
    CHAR  Model[40];
    CHAR  FirmwareRev[8];
    CHAR  Reserved2[8];
    CHAR  TertiaryVersion;
    CHAR  MinorVersion;
    WORD  MajorVersion;
    CHAR  Reserved3[428];
    CHAR  Reserved4[3584];
};
#pragma pack(pop)

struct BIN_IDENTIFY_DEVICE {
    BYTE Bin[4096];
};

union IDENTIFY_DEVICE {
    ATA_IDENTIFY_DEVICE  A;
    NVME_IDENTIFY_DEVICE N;
    BIN_IDENTIFY_DEVICE  B;
};

// ── Enumerations — CrystalDiskInfo CAtaSmart ──────────────────────────

enum VENDOR_ID : DWORD {
    HDD_GENERAL                = 0,
    SSD_GENERAL                = 1,
    SSD_VENDOR_MTRON           = 2,
    SSD_VENDOR_INDILINX        = 3,
    SSD_VENDOR_JMICRON         = 4,
    SSD_VENDOR_INTEL           = 5,
    SSD_VENDOR_SAMSUNG         = 6,
    SSD_VENDOR_SANDFORCE       = 7,
    SSD_VENDOR_MICRON          = 8,
    SSD_VENDOR_OCZ             = 9,
    SSD_VENDOR_SEAGATE         = 10,
    SSD_VENDOR_WDC             = 11,
    SSD_VENDOR_PLEXTOR         = 12,
    SSD_VENDOR_SANDISK         = 13,
    SSD_VENDOR_OCZ_VECTOR      = 14,
    SSD_VENDOR_TOSHIBA         = 15,
    SSD_VENDOR_CORSAIR         = 16,
    SSD_VENDOR_KINGSTON        = 17,
    SSD_VENDOR_MICRON_MU03     = 18,
    SSD_VENDOR_NVME            = 19,
    SSD_VENDOR_REALTEK         = 20,
    SSD_VENDOR_SKHYNIX         = 21,
    SSD_VENDOR_KIOXIA          = 22,
    SSD_VENDOR_SSSTC           = 23,
    SSD_VENDOR_INTEL_DC        = 24,
    SSD_VENDOR_APACER          = 25,
    SSD_VENDOR_SILICONMOTION   = 26,
    SSD_VENDOR_PHISON          = 27,
    SSD_VENDOR_MARVELL         = 28,
    SSD_VENDOR_MAXIOTEK        = 29,
    SSD_VENDOR_YMTC            = 30,
    SSD_VENDOR_SCY             = 31,
    // 32..34 — JMicron controller generations
    SSD_VENDOR_JMICRON_60X     = 32,
    SSD_VENDOR_JMICRON_61X     = 33,
    SSD_VENDOR_JMICRON_66X     = 34,
    SSD_VENDOR_SEAGATE_IRON_WOLF = 35,
    SSD_VENDOR_SEAGATE_BARRA_CUDA = 36,
    SSD_VENDOR_SANDISK_GB      = 37,
    SSD_VENDOR_KINGSTON_SUV    = 38,
    SSD_VENDOR_KINGSTON_KC600  = 39,
    SSD_VENDOR_KINGSTON_DC500  = 40,
    SSD_VENDOR_KINGSTON_SA400  = 41,
    SSD_VENDOR_RECADATA        = 42,
    SSD_VENDOR_SANDISK_DELL    = 43,
    SSD_VENDOR_SANDISK_HP      = 44,
    SSD_VENDOR_SANDISK_HP_VENUS = 45,
    SSD_VENDOR_SANDISK_LENOVO  = 46,
    SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS = 47,
    SSD_VENDOR_SANDISK_CLOUD   = 48,
    SSD_VENDOR_SILICONMOTION_CVC = 49,
    SSD_VENDOR_ADATA_INDUSTRIAL = 50,
    SSD_VENDOR_MAX             = 99,
    VENDOR_UNKNOWN             = 0x0000,
    USB_VENDOR_BUFFALO         = 0x0411,
    USB_VENDOR_IO_DATA         = 0x04BB,
    USB_VENDOR_LOGITEC         = 0x0789,
    USB_VENDOR_INITIO          = 0x13FD,
    USB_VENDOR_SUNPLUS         = 0x04FC,
    USB_VENDOR_JMICRON         = 0x152D,
    USB_VENDOR_CYPRESS         = 0x04B4,
    USB_VENDOR_OXFORD          = 0x0928,
    USB_VENDOR_PROLIFIC        = 0x067B,
    USB_VENDOR_REALTEK         = 0x0BDA,
    USB_VENDOR_ALL             = 0xFFFF,
};

enum INTERFACE_TYPE : DWORD {
    INTERFACE_TYPE_UNKNOWN = 0,
    INTERFACE_TYPE_PATA,
    INTERFACE_TYPE_SATA,
    INTERFACE_TYPE_USB,
    INTERFACE_TYPE_IEEE1394,
    INTERFACE_TYPE_SCSI,
    INTERFACE_TYPE_NVME,
    INTERFACE_TYPE_AMD_RC2,
};

enum COMMAND_TYPE : DWORD {
    CMD_TYPE_UNKNOWN = 0,
    CMD_TYPE_PHYSICAL_DRIVE,
    CMD_TYPE_SCSI_MINIPORT,
    CMD_TYPE_SAT,
    CMD_TYPE_JMICRON,
    CMD_TYPE_NVME_STORAGE_QUERY,
    CMD_TYPE_NVME_INTEL,
    CMD_TYPE_DEBUG,
};

enum DISK_STATUS : DWORD {
    DISK_STATUS_UNKNOWN = 0,
    DISK_STATUS_GOOD,
    DISK_STATUS_CAUTION,
    DISK_STATUS_BAD,
};

enum SMART_STATUS_CHANGE : DWORD {
    SMART_STATUS_NO_CHANGE = 0,
    SMART_STATUS_MINOR_CHANGE,
    SMART_STATUS_MAJOR_CHANGE,
};

enum POWER_ON_HOURS_UNIT : DWORD {
    POWER_ON_UNKNOWN = 0,
    POWER_ON_HOURS,
    POWER_ON_MINUTES,
    POWER_ON_HALF_MINUTES,
    POWER_ON_SECONDS,
    POWER_ON_10_MINUTES,
    POWER_ON_MILLI_SECONDS,
};

enum HOST_READS_WRITES_UNIT : DWORD {
    HOST_READS_WRITES_UNKNOWN = 0,
    HOST_READS_WRITES_512B,
    HOST_READS_WRITES_1MB,
    HOST_READS_WRITES_16MB,
    HOST_READS_WRITES_32MB,
    HOST_READS_WRITES_GB,
};

// ── StorageQuery port (CrystalDiskInfo StorageQuery.h) ────────────────
//
// CrystalDiskInfo hand-rolls these structures so the layout is fixed and
// does not depend on the Windows SDK revision that happens to be used for
// the build.  The same approach is kept here: every structure is declared
// with its documented size so the code compiles and behaves identically
// against the Windows 7 through Windows 11 SDKs.
namespace StorageQuery {

// STORAGE_PROPERTY_ID anchors (see Windows SDK winioctl.h)
constexpr DWORD StorageAdapterProtocolSpecificProperty = 49;
constexpr DWORD StorageDeviceProtocolSpecificProperty  = 50;

// STORAGE_QUERY_TYPE
constexpr DWORD PropertyStandardQuery = 0;

// STORAGE_PROTOCOL_TYPE
constexpr DWORD ProtocolTypeNvme = 3;

// STORAGE_PROTOCOL_NVME_DATA_TYPE
constexpr DWORD NVMeDataTypeUnknown  = 0;
constexpr DWORD NVMeDataTypeIdentify = 1;
constexpr DWORD NVMeDataTypeLogPage  = 2;
constexpr DWORD NVMeDataTypeFeature  = 3;

// _STORAGE_PROTOCOL_SPECIFIC_DATA (40 bytes, stable since Windows 10 1709;
// the trailing three DWORDs were "Reserved" in earlier SDK revisions).
#pragma pack(push, 4)
struct STORAGE_PROTOCOL_SPECIFIC_DATA_CDI {
    DWORD ProtocolType;
    DWORD DataType;
    DWORD ProtocolDataRequestValue;
    DWORD ProtocolDataRequestSubValue;
    DWORD ProtocolDataOffset;
    DWORD ProtocolDataLength;
    DWORD FixedProtocolReturnData;
    DWORD ProtocolDataRequestSubValue2;
    DWORD ProtocolDataRequestSubValue3;
    DWORD ProtocolDataRequestSubValue4;
};
#pragma pack(pop)
static_assert(sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA_CDI) == 40,
              "STORAGE_PROTOCOL_SPECIFIC_DATA_CDI must be 40 bytes");

// CrystalDiskInfo declares its own two-DWORD query rather than using the
// SDK's STORAGE_PROPERTY_QUERY, and it matters: the SDK struct carries a
// trailing AdditionalParameters[1] and therefore has sizeof == 12, which
// places the protocol-specific data four bytes too late.  The storage stack
// expects it immediately after the two DWORDs — with the SDK struct the
// query is rejected with ERROR_INVALID_PARAMETER (87).  Verified on
// Windows 11 with the Microsoft and Samsung NVMe drivers.
struct STORAGE_PROPERTY_QUERY_CDI {
    DWORD PropertyId;
    DWORD QueryType;
};
static_assert(sizeof(STORAGE_PROPERTY_QUERY_CDI) == 8,
              "STORAGE_PROPERTY_QUERY_CDI must be 8 bytes");

struct TStorageQueryWithBuffer {
    STORAGE_PROPERTY_QUERY_CDI              Query;
    STORAGE_PROTOCOL_SPECIFIC_DATA_CDI      ProtocolSpecific;
    BYTE                                    Buffer[4096];
};
// 8 + 40 + 4096 — the same size CrystalDiskInfo passes to DeviceIoControl.
static_assert(sizeof(TStorageQueryWithBuffer) == 4144,
              "TStorageQueryWithBuffer must be 4144 bytes");

} // namespace StorageQuery

// ── SCSI pass-through buffer wrapper ─────────────────────────────────
// The Windows SDK already provides SRB_IO_CONTROL, IDEREGS, SENDCMDINPARAMS,
// SENDCMDOUTPARAMS and SCSI_PASS_THROUGH; only this convenience wrapper is
// needed (CrystalDiskInfo SPTIUtil.h).  Deliberately NOT packed so the
// embedded SCSI_PASS_THROUGH keeps its natural alignment.
struct SCSI_PASS_THROUGH_WITH_BUFFERS {
    SCSI_PASS_THROUGH Spt;
    ULONG             Filler;      // realign buffers to double word boundary
    UCHAR             SenseBuf[32];
    UCHAR             DataBuf[4096];
};

// ATA pass-through buffer wrapper (ATA_PASS_THROUGH_EX + 512 byte data)
struct ATA_PASS_THROUGH_EX_WITH_BUFFERS {
    ATA_PASS_THROUGH_EX Apt;
    BYTE                Buf[512];
};

// ── Per-drive SMART information (CrystalDiskInfo ATA_SMART_INFO subset) ──

struct DRIVE_INFO {
    IDENTIFY_DEVICE  IdentifyDevice{};
    BYTE             SmartReadData[512]{};
    BYTE             SmartReadThreshold[512]{};
    SMART_ATTRIBUTE  Attribute[MAX_ATTRIBUTE]{};
    SMART_THRESHOLD  Threshold[MAX_ATTRIBUTE]{};

    BOOL IsSmartEnabled{};
    BOOL IsSmartSupported{};
    BOOL IsSmartCorrect{};
    BOOL IsThresholdCorrect{};
    BOOL IsCheckSumError{};
    BOOL IsThresholdBug{};
    BOOL IsLba48Supported{};
    BOOL IsSsd{};
    BOOL IsNVMe{};
    BOOL IsUasp{};
    BOOL IsTrimSupported{};
    BOOL IsNvmeThresholdSupported{};
    BOOL IsNvmeThermalManagementSupported{};
    BOOL IsWord88{};

    INT  PhysicalDriveId{};
    INT  ScsiPort{};
    INT  ScsiTargetId{};
    INT  ScsiBus{};
    BYTE Target{ 0xA0 };

    DWORD     TotalDiskSize{};
    DWORD     Cylinder{};
    DWORD     Head{};
    DWORD     Sector{};
    DWORD     Sector28{};
    ULONGLONG Sector48{};
    ULONGLONG NumberOfSectors{};
    DWORD     DiskSizeChs{};
    DWORD     DiskSizeLba28{};
    DWORD     DiskSizeLba48{};
    DWORD     LogicalSectorSize{};
    DWORD     PhysicalSectorSize{};
    DWORD     BufferSize{};
    ULONGLONG NvCacheSize{};

    DWORD TransferModeType{};
    DWORD DetectedTimeUnitType{ POWER_ON_UNKNOWN };
    DWORD MeasuredTimeUnitType{ POWER_ON_UNKNOWN };
    DWORD AttributeCount{};

    INT PowerOnRawValue{};
    DWORD PowerOnCount{};
    INT PowerOnHours{};
    INT Temperature{ -1000 };
    INT TemperatureNVMe[8]{};
    double TemperatureMultiplier{ 1.0 };
    DWORD NominalMediaRotationRate{};

    INT HostWrites{};
    INT HostReads{};
    INT GBytesErased{};
    INT NandWrites{};
    INT WearLevelingCount{ -1 };
    INT Life{ -1 };

    // Byte-accurate host traffic totals used by the IOMonitor throughput
    // graph.  CrystalDiskInfo only keeps HostWrites/HostReads in the unit
    // selected by HostReadsWritesUnit; these two fields carry the value
    // already converted to bytes so callers do not have to know the unit.
    ULONGLONG HostWritesBytes{};
    ULONGLONG HostReadsBytes{};

    DWORD DiskStatus{ DISK_STATUS_UNKNOWN };

    INTERFACE_TYPE InterfaceType{ INTERFACE_TYPE_UNKNOWN };
    COMMAND_TYPE   CommandType{ CMD_TYPE_UNKNOWN };
    DWORD          DiskVendorId{ HDD_GENERAL };
    DWORD          UsbVendorId{};
    DWORD          UsbProductId{};
    HOST_READS_WRITES_UNIT HostReadsWritesUnit{ HOST_READS_WRITES_UNKNOWN };

    WORD Threshold05{};
    WORD ThresholdC5{};
    WORD ThresholdC6{};
    WORD ThresholdFF{};

    std::wstring SerialNumber;
    std::wstring FirmwareRev;
    std::wstring Model;
    std::wstring Interface;
    std::wstring CommandTypeString;
    std::wstring SsdVendorString;
    std::wstring DeviceNominalFormFactor;
    std::wstring PnpDeviceId;
    std::wstring TransferMode;
    std::wstring SmartKeyName{ L"Smart" };
};

// ── NVMe Interpreter (CrystalDiskInfo NVMeInterpreter.cpp) ────────────
//
// Translates the 512-byte NVMe SMART / Health Information Log into the
// ATA-style attribute list that the CrystalDiskInfo attribute tables
// describe (ids 0x01..0x1D in log-field order, see [SmartNVMe]).
//
// CrystalDiskInfo splits the translation into four helpers that only
// differ in which temperature source becomes the primary "Temperature"
// attribute.  Because every NVMe log exposes the composite temperature,
// all eight sensors and the thermal-management counters at once, the port
// folds them into a single entry point that emits the complete list.
//
// Returns the number of attributes written (0 on failure).
DWORD NVMeSmartToATASmart(const BYTE* nvmeSmartBuf, SMART_ATTRIBUTE* out, DWORD capacity);

// Temperature source variants kept for API parity with CrystalDiskInfo.
DWORD NVMeCompositeTemperatureSmartToATASmart(const BYTE* nvmeSmartBuf, SMART_ATTRIBUTE* out, DWORD capacity);
DWORD NVMeTemperatureSensorSmartToATASmart(const BYTE* nvmeSmartBuf, SMART_ATTRIBUTE* out, DWORD capacity);
DWORD NVMeThermalManagementTemperatureSmartToATASmart(const BYTE* nvmeSmartBuf, SMART_ATTRIBUTE* out, DWORD capacity);

// ── Little endian byte helpers (CrystalDiskInfo B8toBxx) ──────────────
inline WORD B8toB16le(const BYTE* b) {
    return static_cast<WORD>(b[0] | (b[1] << 8));
}
inline DWORD B8toB32le(const BYTE* b) {
    return static_cast<DWORD>(b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24));
}
inline ULONGLONG B8toB64le(const BYTE* b) {
    ULONGLONG v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
    return v;
}
inline WORD B8toB16le_ptr(const BYTE* p) { return B8toB16le(p); }

// ATA strings are stored word-swapped and space padded
void ChangeByteOrder(char* str, DWORD length);

// ── CrystalDiskInfo CAtaSmart port ────────────────────────────────────

class CAtaSmart {
public:
    CAtaSmart();
    ~CAtaSmart();

    CAtaSmart(const CAtaSmart&) = delete;
    CAtaSmart& operator=(const CAtaSmart&) = delete;

    // Enumerate every \\\\.\\PhysicalDriveN and collect S.M.A.R.T. data.
    // Mirrors CAtaSmart::Init() for the mainstream Windows 10/11 paths.
    BOOL Init(BOOL advancedDiskSearch);

    // Re-read identity + attributes + thresholds for one drive.
    DWORD UpdateSmartInfo(DWORD index);
    BOOL  UpdateIdInfo(DWORD index);

    DWORD GetDiskCount() const { return static_cast<DWORD>(m_vars.size()); }
    DRIVE_INFO&       GetDisk(DWORD index)       { return m_vars[index]; }
    const DRIVE_INFO& GetDisk(DWORD index) const { return m_vars[index]; }
    std::vector<DRIVE_INFO>& Disks()             { return m_vars; }

    // Enable / disable S.M.A.R.T. and query overall status (SMART RETURN STATUS)
    BOOL EnableSmart(DWORD index);
    BOOL DisableSmart(DWORD index);
    BYTE GetAamValue(DWORD index);
    BYTE GetApmValue(DWORD index);

    // CrystalDiskInfo CAtaSmart::CheckDiskStatus()
    static DWORD CheckDiskStatus(const DRIVE_INFO& asi);

    // CrystalDiskInfo CAtaSmart::GetPowerOnHours()
    static DWORD GetPowerOnHours(DWORD rawValue, DWORD timeUnitType);

    // CrystalDiskInfo CAtaSmart::MeasuredTimeUnit() — refines each drive's
    // MeasuredTimeUnitType by sampling PowerOnRawValue across a 125..155
    // second window.  Call it from a timer after the first refresh; the
    // values it produces are what CdiAttributeName.cpp formats from.
    void MeasuredTimeUnit();

private:
    // ── Physical drive helpers ────────────────────────────────────────
    // Upstream's GetScsiHandle() built a \\.\ScsiN: path for the SRB_IO_CONTROL
    // miniport protocol; that path is out of scope, so neither the helper nor
    // the m_bScsiMiniport flag it guarded exists here.
    static HANDLE GetIoCtrlHandle(INT index);

    // Probe a single \\\\.\\PhysicalDriveN and add it on success
    BOOL DetectDisk(INT physicalDriveId);

    BOOL AddDisk(INT physicalDriveId, BYTE target, COMMAND_TYPE commandType,
                 IDENTIFY_DEVICE* identify, const STORAGE_DEVICE_DESCRIPTOR* desc,
                 const std::wstring& pnpDeviceId);
    BOOL AddDiskNVMe(INT physicalDriveId, IDENTIFY_DEVICE* identify,
                     const STORAGE_DEVICE_DESCRIPTOR* desc, DWORD diskSize,
                     const std::wstring& pnpDeviceId);

    // ── \\\\.\\PhysicalDriveX paths ───────────────────────────────────
    BOOL DoIdentifyDevicePd(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data);
    BOOL GetSmartAttributePd(INT physicalDriveId, BYTE target, DRIVE_INFO* asi);
    BOOL GetSmartThresholdPd(INT physicalDriveId, BYTE target, DRIVE_INFO* asi);
    BOOL ControlSmartStatusPd(INT physicalDriveId, BYTE target, BYTE command);
    BOOL ReadLogExtPd(INT physicalDriveId, BYTE target, BYTE logAddress, BYTE logPage,
                      BYTE* data, DWORD dataSize);
    BOOL SendAtaCommandPd(INT physicalDriveId, BYTE target, BYTE main, BYTE sub,
                          BYTE param, BYTE* data, DWORD dataSize);

    // ── SCSI ATA PASS-THROUGH (12) / SAT paths ────────────────────────
    BOOL DoIdentifyDeviceSat(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data,
                             COMMAND_TYPE type);
    BOOL GetSmartAttributeSat(INT physicalDriveId, BYTE target, DRIVE_INFO* asi);
    BOOL GetSmartThresholdSat(INT physicalDriveId, BYTE target, DRIVE_INFO* asi);
    BOOL ControlSmartStatusSat(INT physicalDriveId, BYTE target, BYTE command,
                               COMMAND_TYPE type);
    BOOL SendAtaCommandSat(INT physicalDriveId, BYTE target, BYTE main, BYTE sub,
                           BYTE param, COMMAND_TYPE type);

    // ── Legacy ATA SMART fallback: SCSI miniport SMART_RCV_DRIVE_DATA ──
    BOOL DoIdentifyDeviceSi(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data);
    BOOL GetSmartAttributeSi(INT physicalDriveId, DRIVE_INFO* asi);
    BOOL GetSmartThresholdSi(INT physicalDriveId, DRIVE_INFO* asi);

    // Send SMART_RCV_DRIVE_DATA (0x07C088) through IOCTL_SCSI_MINIPORT
    BOOL SendSmartRcvDriveDataSi(INT physicalDriveId, BYTE subCommand,
                                 BYTE* data, DWORD dataSize);

    // ── NVMe via Storage Protocol Specific Query (Windows 10+) ────────
    BOOL DoIdentifyDeviceNVMeStorageQuery(INT physicalDriveId, IDENTIFY_DEVICE* data,
                                          DWORD* diskSize);
    BOOL GetSmartAttributeNVMeStorageQuery(INT physicalDriveId, DRIVE_INFO* asi);
    BOOL GetNvMeIdentifyControllerData(INT physicalDriveId, BYTE* outBuffer);
    static BOOL IsNVMeTemperatureThresholdDefined(BYTE* identifyControllerData);
    static BOOL IsNVMeThermalManagementTemperatureDefined(BYTE* identifyControllerData);

    // ── Post-processing / health ──────────────────────────────────────
    BOOL  FillSmartData(DRIVE_INFO* asi);
    BOOL  FillSmartThreshold(DRIVE_INFO* asi);
    void  CheckSsdSupport(DRIVE_INFO& asi);
    void  DetectTimeUnit(DRIVE_INFO& asi);
    void  SetInterfaceType(DRIVE_INFO& asi, const STORAGE_DEVICE_DESCRIPTOR* desc);

    std::vector<DRIVE_INFO> m_vars;
    BOOL m_bAtaPassThrough{ FALSE };
    BOOL m_bAtaPassThroughSmart{ FALSE };
    BOOL m_bNVMeStorageQuery{ FALSE };
    BOOL m_IsAdvancedDiskSearch{ FALSE };
};

} // namespace cdi
