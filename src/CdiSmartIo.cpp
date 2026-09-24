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

// ═══════════════════════════════════════════════════════════════════════
//  CdiSmartIo — the S.M.A.R.T. acquisition paths of CAtaSmart
//
//  Ported from CrystalDiskInfo (MIT License, Copyright (c) hiyohiyo)
//  AtaSmart.cpp.  Upstream line ranges, one per ported function:
//
//    \\.\PhysicalDriveN
//      CAtaSmart::GetIoCtrlHandle() ................ AtaSmart.cpp  6988- 6996
//      CAtaSmart::DoIdentifyDevicePd() ............. AtaSmart.cpp  6998- 7054
//      CAtaSmart::GetSmartAttributePd() ............ AtaSmart.cpp  7056- 7109
//      CAtaSmart::GetSmartThresholdPd() ............ AtaSmart.cpp  7111- 7164
//      CAtaSmart::ControlSmartStatusPd() ........... AtaSmart.cpp  7166- 7211
//      CAtaSmart::ReadLogExtPd() ................... AtaSmart.cpp  7213- 7272
//      CAtaSmart::SendAtaCommandPd() ............... AtaSmart.cpp  7273- 7354
//
//    SCSI ATA PASS-THROUGH (12) — the SAT / JMicron bridge paths
//      CAtaSmart::DoIdentifyDeviceSat() ............ AtaSmart.cpp  9342- 9607
//      CAtaSmart::GetSmartAttributeSat() ........... AtaSmart.cpp  9609- 9817
//      CAtaSmart::GetSmartThresholdSat() ........... AtaSmart.cpp  9819-10017
//      CAtaSmart::ControlSmartStatusSat() .......... AtaSmart.cpp 10019-10204
//      CAtaSmart::SendAtaCommandSat() .............. AtaSmart.cpp 10206-10396
//
//    Legacy ATA/SMART IOCTL fallback (DFP_RECEIVE_DRIVE_DATA)
//      CAtaSmart::SendSmartRcvDriveDataSi() ........ factored out of the three
//                                                    identical DFP exchanges at
//                                                    AtaSmart.cpp  7013- 7052,
//                                                                  7080- 7108,
//                                                                  7135- 7163
//      CAtaSmart::DoIdentifyDeviceSi() ............. AtaSmart.cpp 10502-10535
//                                                    (interface only — upstream
//                                                    drives a CMD_IDE miniport
//                                                    there, see below)
//      CAtaSmart::GetSmartAttributeSi() ............ AtaSmart.cpp 10537-10560
//      CAtaSmart::GetSmartThresholdSi() ............ AtaSmart.cpp 10562-10564
//
//    NVMe via Storage Protocol Specific Query
//      CAtaSmart::DoIdentifyDeviceNVMeStorageQuery() ... AtaSmart.cpp 8958- 9008
//      CAtaSmart::GetSmartAttributeNVMeStorageQuery() .. AtaSmart.cpp 9010- 9052
//      CAtaSmart::GetNvMeIdentifyControllerData() ...... AtaSmart.cpp 9054- 9090
//      CAtaSmart::IsNVMeTemperatureThresholdDefined() .. AtaSmart.cpp 9092- 9098
//      CAtaSmart::IsNVMeThermalManagementTemperatureDefined()
//                                                    ... AtaSmart.cpp 9100- 9106
//
//  Deliberately OUT OF SCOPE — these never run on a stock Windows 10 / 11
//  installation and CdiSmart.h carries neither the structures nor the
//  COMMAND_TYPE values they need:
//    * the \\.\ScsiN: SRB_IO_CONTROL miniport paths — upstream's
//      GetIoCtrlHandle(scsiPort, siliconImageType), DoIdentifyDeviceScsi /
//      GetSmartAttributeScsi / GetSmartThresholdScsi / ControlSmartStatusScsi
//      / SendAtaCommandScsi (AtaSmart.cpp 9113-9340), the CSMI
//      (AddDiskCsmi / CsmiIoctl), MegaRAID, Intel-RST, Intel-VROC, Silicon
//      Image, AMD-RC2 and JMS56X / JMB39X / JMS586 / JMS59X USB bridge paths,
//      and the IOCTL_SCSI_MINIPORT SMART_RCV_DRIVE_DATA exchange that upstream
//      uses inside DoIdentifyDeviceSi / GetSmartAttributeSi.
//    * every vendor-private NVMe tunnel (JMicron, ASMedia, Realtek, Samsung,
//      Intel, SiliconMotion, ...), i.e. AtaSmart.cpp 7448-8957.
//    * the IOCTL_STORAGE_PREDICT_FAILURE alternate and the WMI fallback
//      (GetSmartAttributeWmi / GetSmartThresholdWmi / GetSmartInfoWmi) that
//      upstream's GetSmartAttributeSi / GetSmartThresholdSi fall back to.
//    * the per-function branches keyed on the ASM1352R / REALTEK9220DP /
//      SUNPLUS / IO_DATA / LOGITEC / PROLIFIC / CYPRESS COMMAND_TYPE values:
//      those enumerators do not exist in CdiSmart.h, so each SAT function
//      keeps only its CMD_TYPE_SAT and CMD_TYPE_JMICRON branches.
//
//  The port is byte-for-byte faithful to the upstream control flow; every
//  remaining difference is called out in a comment and listed in the unit's
//  report.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmart.h"
#include "CdiSmartDetail.h"
#include "SmartDebug.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace cdi {

static_assert(sizeof(NVME_IDENTIFY_DEVICE) == 4096,
              "NVME_IDENTIFY_DEVICE must be 4096 bytes");

// ── Packed IOCTL structures (upstream AtaSmart.h:414-431, verbatim) ────

#pragma pack(push,1)

struct IDENTIFY_DEVICE_OUTDATA
{
    SENDCMDOUTPARAMS SendCmdOutParam;
    BYTE             Data[IDENTIFY_BUFFER_SIZE - 1];
};

struct SMART_READ_DATA_OUTDATA
{
    SENDCMDOUTPARAMS SendCmdOutParam;
    BYTE             Data[READ_ATTRIBUTE_BUFFER_SIZE - 1];
};

struct CMD_IDE_PATH_THROUGH
{
    IDEREGS reg;
    DWORD   length;
    BYTE    buffer[1];
};

#pragma pack(pop)

// The two OUTPUT buffers the legacy DFP_RECEIVE_DRIVE_DATA exchange requires:
// one transfer of cBufferSize bytes starting at SENDCMDOUTPARAMS::bBuffer.
static_assert(sizeof(SENDCMDOUTPARAMS) == 17,
              "SENDCMDOUTPARAMS must stay packed (upstream IDENTIFY_DEVICE_OUTDATA ABI)");
static_assert(sizeof(IDENTIFY_DEVICE_OUTDATA) == 528,
              "IDENTIFY_DEVICE_OUTDATA must be 528 bytes");
static_assert(sizeof(SMART_READ_DATA_OUTDATA) == 528,
              "SMART_READ_DATA_OUTDATA must be 528 bytes");
static_assert(sizeof(CMD_IDE_PATH_THROUGH) == 13,
              "CMD_IDE_PATH_THROUGH must be 13 bytes");

// ── ATA task file byte order ──────────────────────────────────────────
//
// The Windows SDK exposes the two ATA_PASS_THROUGH_EX task files as
// UCHAR PreviousTaskFile[8] / UCHAR CurrentTaskFile[8].  Upstream CrystalDiskInfo
// (AtaSmart.h:441-459) declares its own copy of the struct whose task files are
// typed IDEREGS, which is why its code can say `.CurrentTaskFile.bFeaturesReg`.
// The byte order is identical to IDEREGS, so the named form maps onto the array
// indices below.
enum AtaTaskFileIndex
{
    TF_FEATURES      = 0,   // IDEREGS::bFeaturesReg
    TF_SECTOR_COUNT  = 1,   // IDEREGS::bSectorCountReg
    TF_SECTOR_NUMBER = 2,   // IDEREGS::bSectorNumberReg
    TF_CYL_LOW       = 3,   // IDEREGS::bCylLowReg
    TF_CYL_HIGH      = 4,   // IDEREGS::bCylHighReg
    TF_DRIVE_HEAD    = 5,   // IDEREGS::bDriveHeadReg
    TF_COMMAND       = 6,   // IDEREGS::bCommandReg
    TF_RESERVED      = 7,   // IDEREGS::bReserved
};

namespace {

// ── Local helpers ─────────────────────────────────────────────────────

// Replaces upstream's safeCloseHandle().  Upstream closes the device handle
// explicitly at selected exit points (and leaks it on two early returns inside
// ReadLogExtPd / SendAtaCommandPd); a scope guard closes it on every path.
class HandleCloser
{
public:
    explicit HandleCloser(HANDLE h) : m_h(h) {}
    ~HandleCloser() { close(); }

    HandleCloser(const HandleCloser&) = delete;
    HandleCloser& operator=(const HandleCloser&) = delete;

    void close()
    {
        if (m_h != NULL && m_h != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_h);
            m_h = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE m_h;
};

// Replaces upstream's safeVirtualFree().
void SafeVirtualFree(LPVOID p)
{
    if (p != NULL)
    {
        ::VirtualFree(p, 0, MEM_RELEASE);
    }
}

// Builds the narrow message for SMART_TRACE_EVENT() where upstream built a
// CString with DebugPrint()/CString::Format().
std::string TraceFmt(const char* fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return std::string(buf);
}

// ── NVMe protocol query with property-id fallback ────────────────────
//
// CrystalDiskInfo only ever asks for StorageAdapterProtocolSpecificProperty
// (49).  The Samsung NVMe driver stack on Windows 11 answers 49 with
// ERROR_INVALID_FUNCTION while accepting the device-level
// StorageDeviceProtocolSpecificProperty (50).  Both return the same
// STORAGE_PROTOCOL_DATA_DESCRIPTOR with the payload at the same offset, so
// the port retries at the device level before giving up.  Verified on
// Windows 11 26200 with SAMSUNG MZVL21T0HCLR-00B00 devices: 49 -> error 1,
// 50 -> the SMART health log page is returned and parses correctly.
//
// Only Query.PropertyId is touched, so the caller's ProtocolSpecific payload
// is preserved across the retry and the buffer can be re-sent verbatim.
BOOL StorageQueryIoControl(HANDLE hIoCtrl, StorageQuery::TStorageQueryWithBuffer* nptwb,
                           DWORD* dwReturned)
{
    static const DWORD kPropertyIds[2] = {
        StorageQuery::StorageAdapterProtocolSpecificProperty,   // 49
        StorageQuery::StorageDeviceProtocolSpecificProperty,    // 50
    };

    for (int i = 0; i < 2; ++i)
    {
        nptwb->Query.PropertyId = kPropertyIds[i];

        DWORD returned = 0;
        if (::DeviceIoControl(hIoCtrl, IOCTL_STORAGE_QUERY_PROPERTY,
                              nptwb, sizeof(*nptwb), nptwb, sizeof(*nptwb),
                              &returned, NULL))
        {
            if (dwReturned != NULL) *dwReturned = returned;
            return TRUE;
        }
    }
    return FALSE;
}

} // anonymous namespace

/*---------------------------------------------------------------------------*/
//  \\.\PhysicalDriveN
/*---------------------------------------------------------------------------*/

// upstream AtaSmart.cpp:6988-6996
HANDLE CAtaSmart::GetIoCtrlHandle(INT/*BYTE*/ index)
{
    const std::wstring strDevice = detail::FormatW(L"\\\\.\\PhysicalDrive%d", index);

    return ::CreateFile(strDevice.c_str(), GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
}

// upstream AtaSmart.cpp:6998-7054
BOOL CAtaSmart::DoIdentifyDevicePd(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    IDENTIFY_DEVICE_OUTDATA sendCmdOutParam = {};
    SENDCMDINPARAMS         sendCmd = {};

    if (data == NULL)
    {
        return FALSE;
    }

    if (m_bAtaPassThrough && m_bAtaPassThroughSmart)
    {
        SMART_TRACE_EVENT("DoIdentifyDevicePd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - IDENTIFY_DEVICE (ATA_PASS_THROUGH)");
        bRet = SendAtaCommandPd(physicalDriveId, target, 0xEC, 0x00, 0x00,
                                (BYTE*)data, sizeof(ATA_IDENTIFY_DEVICE));
    }

    // upstream quirk: the pass-through result is validated by inspecting the
    // (still word swapped) Model field — upstream assigns it to a CString and
    // tests IsEmpty(), which is the same as testing the first byte for NUL.
    // When the pass-through branch did not run, upstream's cstr stays empty and
    // this fallback always executes.
    if (bRet == FALSE || data->A.Model[0] == '\0')
    {
        // upstream quirk: upstream calls ::ZeroMemory(data,
        // sizeof(ATA_IDENTIFY_DEVICE)) here, which clears only the first 512
        // bytes of the 4096 byte IDENTIFY_DEVICE union and would leave the
        // NVMe region of the buffer stale.  The caller pre-zeroes the whole
        // union, so no partial clear is performed.
        HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
        if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
        {
            return FALSE;
        }
        HandleCloser closer(hIoCtrl);

        //::ZeroMemory(&sendCmdOutParam, sizeof(IDENTIFY_DEVICE_OUTDATA));
        //::ZeroMemory(&sendCmd, sizeof(SENDCMDINPARAMS));

        sendCmd.irDriveRegs.bCommandReg        = ID_CMD;
        sendCmd.irDriveRegs.bSectorCountReg    = 1;
        sendCmd.irDriveRegs.bSectorNumberReg   = 1;
        sendCmd.irDriveRegs.bDriveHeadReg      = target;
        sendCmd.cBufferSize                    = IDENTIFY_BUFFER_SIZE;

        SMART_TRACE_EVENT("DoIdentifyDevicePd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - IDENTIFY_DEVICE");
        bRet = ::DeviceIoControl(hIoCtrl, DFP_RECEIVE_DRIVE_DATA,
                                 &sendCmd, sizeof(SENDCMDINPARAMS),
                                 &sendCmdOutParam, sizeof(IDENTIFY_DEVICE_OUTDATA),
                                 &dwReturned, NULL);

        if (bRet == FALSE || dwReturned != sizeof(IDENTIFY_DEVICE_OUTDATA))
        {
            return FALSE;
        }

        ::memcpy_s(data, sizeof(ATA_IDENTIFY_DEVICE),
                   sendCmdOutParam.SendCmdOutParam.bBuffer, sizeof(ATA_IDENTIFY_DEVICE));
    }

    return TRUE;
}

// upstream AtaSmart.cpp:7056-7109
BOOL CAtaSmart::GetSmartAttributePd(INT physicalDriveId, BYTE target, DRIVE_INFO* asi)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    SMART_READ_DATA_OUTDATA sendCmdOutParam = {};
    SENDCMDINPARAMS         sendCmd = {};

    if (m_bAtaPassThrough && m_bAtaPassThroughSmart)
    {
        SMART_TRACE_EVENT("GetSmartAttributePd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_READ_DATA (ATA_PASS_THROUGH)");
        bRet = SendAtaCommandPd(physicalDriveId, target, SMART_CMD, READ_ATTRIBUTES, 0x00,
                                (BYTE*)asi->SmartReadData, sizeof(asi->SmartReadData));
    }

    if (!bRet)
    {
        HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
        if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
        {
            return FALSE;
        }
        HandleCloser closer(hIoCtrl);

        //::ZeroMemory(&sendCmdOutParam, sizeof(SMART_READ_DATA_OUTDATA));
        //::ZeroMemory(&sendCmd, sizeof(SENDCMDINPARAMS));

        sendCmd.irDriveRegs.bFeaturesReg       = READ_ATTRIBUTES;
        sendCmd.irDriveRegs.bSectorCountReg    = 1;
        sendCmd.irDriveRegs.bSectorNumberReg   = 1;
        sendCmd.irDriveRegs.bCylLowReg         = SMART_CYL_LOW;
        sendCmd.irDriveRegs.bCylHighReg        = SMART_CYL_HI;
        sendCmd.irDriveRegs.bDriveHeadReg      = target;
        sendCmd.irDriveRegs.bCommandReg        = SMART_CMD;
        sendCmd.cBufferSize                    = READ_ATTRIBUTE_BUFFER_SIZE;

        SMART_TRACE_EVENT("GetSmartAttributePd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_READ_DATA");
        bRet = ::DeviceIoControl(hIoCtrl, DFP_RECEIVE_DRIVE_DATA,
                                 &sendCmd, sizeof(SENDCMDINPARAMS),
                                 &sendCmdOutParam, sizeof(SMART_READ_DATA_OUTDATA),
                                 &dwReturned, NULL);

        if (bRet == FALSE || dwReturned != sizeof(SMART_READ_DATA_OUTDATA))
        {
            return FALSE;
        }

        ::memcpy_s(&(asi->SmartReadData), 512,
                   &(sendCmdOutParam.SendCmdOutParam.bBuffer), 512);
    }

    return FillSmartData(asi);
}

// upstream AtaSmart.cpp:7111-7164
BOOL CAtaSmart::GetSmartThresholdPd(INT physicalDriveId, BYTE target, DRIVE_INFO* asi)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    SMART_READ_DATA_OUTDATA sendCmdOutParam = {};
    SENDCMDINPARAMS         sendCmd = {};

    if (m_bAtaPassThrough && m_bAtaPassThroughSmart)
    {
        SMART_TRACE_EVENT("GetSmartThresholdPd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_READ_THRESHOLDS (ATA_PASS_THROUGH)");
        bRet = SendAtaCommandPd(physicalDriveId, target, SMART_CMD, READ_THRESHOLDS, 0x00,
                                (BYTE*)asi->SmartReadThreshold, sizeof(asi->SmartReadThreshold));
    }

    if (!bRet)
    {
        HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
        if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
        {
            return FALSE;
        }
        HandleCloser closer(hIoCtrl);

        //::ZeroMemory(&sendCmdOutParam, sizeof(SMART_READ_DATA_OUTDATA));
        //::ZeroMemory(&sendCmd, sizeof(SENDCMDINPARAMS));

        sendCmd.irDriveRegs.bFeaturesReg       = READ_THRESHOLDS;
        sendCmd.irDriveRegs.bSectorCountReg    = 1;
        sendCmd.irDriveRegs.bSectorNumberReg   = 1;
        sendCmd.irDriveRegs.bCylLowReg         = SMART_CYL_LOW;
        sendCmd.irDriveRegs.bCylHighReg        = SMART_CYL_HI;
        sendCmd.irDriveRegs.bDriveHeadReg      = target;
        sendCmd.irDriveRegs.bCommandReg        = SMART_CMD;
        sendCmd.cBufferSize                    = READ_THRESHOLD_BUFFER_SIZE;

        SMART_TRACE_EVENT("GetSmartThresholdPd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_READ_THRESHOLDS");
        bRet = ::DeviceIoControl(hIoCtrl, DFP_RECEIVE_DRIVE_DATA,
                                 &sendCmd, sizeof(SENDCMDINPARAMS),
                                 &sendCmdOutParam, sizeof(SMART_READ_DATA_OUTDATA),
                                 &dwReturned, NULL);

        if (bRet == FALSE || dwReturned != sizeof(SMART_READ_DATA_OUTDATA))
        {
            return FALSE;
        }

        ::memcpy_s(&(asi->SmartReadThreshold), 512,
                   &(sendCmdOutParam.SendCmdOutParam.bBuffer), 512);
    }

    return FillSmartThreshold(asi);
}

// upstream AtaSmart.cpp:7166-7211
BOOL CAtaSmart::ControlSmartStatusPd(INT physicalDriveId, BYTE target, BYTE command)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    SENDCMDINPARAMS    sendCmd = {};
    SENDCMDOUTPARAMS   sendCmdOutParam = {};

    if (m_bAtaPassThrough && m_bAtaPassThroughSmart)
    {
        SMART_TRACE_EVENT("ControlSmartStatusPd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_CONTROL_STATUS (ATA_PASS_THROUGH)");
        bRet = SendAtaCommandPd(physicalDriveId, target, SMART_CMD, command, 0x00, NULL, 0);
    }

    if (!bRet)
    {
        SMART_TRACE_EVENT("ControlSmartStatusPd", SmartTraceCategory::READER_IOCTL,
                          "SendAtaCommandPd - SMART_CONTROL_STATUS");
        HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
        if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
        {
            return FALSE;
        }
        HandleCloser closer(hIoCtrl);

        //::ZeroMemory(&sendCmd, sizeof(SENDCMDINPARAMS));
        //::ZeroMemory(&sendCmdOutParam, sizeof(SENDCMDOUTPARAMS));

        sendCmd.irDriveRegs.bFeaturesReg       = command;
        sendCmd.irDriveRegs.bSectorCountReg    = 1;
        sendCmd.irDriveRegs.bSectorNumberReg   = 1;
        sendCmd.irDriveRegs.bCylLowReg         = SMART_CYL_LOW;
        sendCmd.irDriveRegs.bCylHighReg        = SMART_CYL_HI;
        sendCmd.irDriveRegs.bDriveHeadReg      = target;
        sendCmd.irDriveRegs.bCommandReg        = SMART_CMD;
        sendCmd.cBufferSize                    = 0;

        bRet = ::DeviceIoControl(hIoCtrl, DFP_SEND_DRIVE_COMMAND,
                                 &sendCmd, sizeof(SENDCMDINPARAMS) - 1,
                                 &sendCmdOutParam, sizeof(SENDCMDOUTPARAMS) - 1,
                                 &dwReturned, NULL);
    }

    return bRet;
}

// upstream AtaSmart.cpp:7213-7272
BOOL CAtaSmart::ReadLogExtPd(INT physicalDriveId, BYTE target, BYTE logAddress, BYTE logPage,
                             BYTE* data, DWORD dataSize)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //if (TRUE || m_bAtaPassThrough) // *Always TRUE if
    {
        ATA_PASS_THROUGH_EX_WITH_BUFFERS ab = {};
        //::ZeroMemory(&ab, sizeof(ab));
        ab.Apt.Length = sizeof(ATA_PASS_THROUGH_EX);
        ab.Apt.TimeOutValue = 2;
        DWORD size = static_cast<DWORD>(offsetof(ATA_PASS_THROUGH_EX_WITH_BUFFERS, Buf));
        ab.Apt.DataBufferOffset = size;

        if (dataSize > 0)
        {
            if (dataSize > sizeof(ab.Buf))
            {
                return FALSE;
            }
            ab.Apt.AtaFlags = ATA_FLAGS_DATA_IN | ATA_FLAGS_48BIT_COMMAND;
            ab.Apt.DataTransferLength = dataSize;
            ab.Buf[0] = 0xCF; // magic number
            size += dataSize;
        }

        ab.Apt.CurrentTaskFile[TF_FEATURES]      = 0x00;
        ab.Apt.CurrentTaskFile[TF_SECTOR_COUNT]  = 0x01;
        ab.Apt.CurrentTaskFile[TF_SECTOR_NUMBER] = logAddress;  // Log Address
        ab.Apt.CurrentTaskFile[TF_CYL_LOW]       = logPage;     // Page #
        ab.Apt.CurrentTaskFile[TF_CYL_HIGH]      = 0;
        ab.Apt.CurrentTaskFile[TF_DRIVE_HEAD]    = target;
        ab.Apt.CurrentTaskFile[TF_COMMAND]       = 0x2F;        // Read Log Ext

        ab.Apt.PreviousTaskFile[TF_FEATURES]      = 0;
        ab.Apt.PreviousTaskFile[TF_SECTOR_COUNT]  = 0;
        ab.Apt.PreviousTaskFile[TF_SECTOR_NUMBER] = 0;
        ab.Apt.PreviousTaskFile[TF_CYL_LOW]       = 0;
        ab.Apt.PreviousTaskFile[TF_CYL_HIGH]      = 0;
        ab.Apt.PreviousTaskFile[TF_DRIVE_HEAD]    = 0;
        ab.Apt.PreviousTaskFile[TF_COMMAND]       = 0;

        bRet = ::DeviceIoControl(hIoCtrl, IOCTL_ATA_PASS_THROUGH,
                                 &ab, size, &ab, size, &dwReturned, NULL);
        if (bRet && dataSize && data != NULL)
        {
            ::memcpy_s(data, dataSize, ab.Buf, dataSize);
        }
    }

    return bRet;
}

// upstream AtaSmart.cpp:7273-7354
BOOL CAtaSmart::SendAtaCommandPd(INT physicalDriveId, BYTE target, BYTE main, BYTE sub,
                                 BYTE param, BYTE* data, DWORD dataSize)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    if (m_bAtaPassThrough)
    {
        ATA_PASS_THROUGH_EX_WITH_BUFFERS ab = {};
        //::ZeroMemory(&ab, sizeof(ab));
        ab.Apt.Length = sizeof(ATA_PASS_THROUGH_EX);
        ab.Apt.TimeOutValue = 2;
        DWORD size = static_cast<DWORD>(offsetof(ATA_PASS_THROUGH_EX_WITH_BUFFERS, Buf));
        ab.Apt.DataBufferOffset = size;

        if (dataSize > 0)
        {
            if (dataSize > sizeof(ab.Buf))
            {
                return FALSE;
            }
            ab.Apt.AtaFlags = ATA_FLAGS_DATA_IN;
            ab.Apt.DataTransferLength = dataSize;
            ab.Buf[0] = 0xCF; // magic number
            size += dataSize;
        }

        ab.Apt.CurrentTaskFile[TF_FEATURES]      = sub;
        ab.Apt.CurrentTaskFile[TF_SECTOR_COUNT]  = param;
        ab.Apt.CurrentTaskFile[TF_DRIVE_HEAD]    = target;
        ab.Apt.CurrentTaskFile[TF_COMMAND]       = main;

        if (main == SMART_CMD)
        {
            ab.Apt.CurrentTaskFile[TF_CYL_LOW]       = SMART_CYL_LOW;
            ab.Apt.CurrentTaskFile[TF_CYL_HIGH]      = SMART_CYL_HI;
            ab.Apt.CurrentTaskFile[TF_SECTOR_COUNT]  = 1;
            ab.Apt.CurrentTaskFile[TF_SECTOR_NUMBER] = 1;
        }

        bRet = ::DeviceIoControl(hIoCtrl, IOCTL_ATA_PASS_THROUGH,
                                 &ab, size, &ab, size, &dwReturned, NULL);
        if (bRet && dataSize && data != NULL)
        {
            ::memcpy_s(data, dataSize, ab.Buf, dataSize);
        }
    }
    else
    {
        // upstream quirk: MEM_COMMIT without MEM_RESERVE, and the copy-back
        // below runs even when VirtualAlloc failed — bRet is still FALSE in
        // that case, so buf is never dereferenced.
        DWORD size = sizeof(CMD_IDE_PATH_THROUGH) - 1 + dataSize;
        CMD_IDE_PATH_THROUGH* buf =
            (CMD_IDE_PATH_THROUGH*)::VirtualAlloc(NULL, size, MEM_COMMIT, PAGE_READWRITE);
        if (buf != NULL)
        {
            buf->reg.bFeaturesReg     = sub;
            buf->reg.bSectorCountReg  = param;
            buf->reg.bSectorNumberReg = 0;
            buf->reg.bCylLowReg       = 0;
            buf->reg.bCylHighReg      = 0;
            buf->reg.bDriveHeadReg    = target;
            buf->reg.bCommandReg      = main;
            buf->reg.bReserved        = 0;
            buf->length               = dataSize;

            bRet = ::DeviceIoControl(hIoCtrl, IOCTL_IDE_PASS_THROUGH_CDI,
                                     buf, size, buf, size, &dwReturned, NULL);
        }
        if (bRet && dataSize && data != NULL)
        {
            ::memcpy_s(data, dataSize, buf->buffer, dataSize);
        }
        SafeVirtualFree(buf);
    }

    return bRet;
}

/*---------------------------------------------------------------------------*/
//  SCSI ATA PASS-THROUGH (12) — SAT
/*---------------------------------------------------------------------------*/

// upstream AtaSmart.cpp:9342-9607 (CMD_TYPE_SAT / CMD_TYPE_JMICRON only)
BOOL CAtaSmart::DoIdentifyDeviceSat(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data,
                                    COMMAND_TYPE type)
{
    SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL,
                      TraceFmt("DoIdentifyDeviceSat pd=%d, tt=%d, ct=%d",
                               physicalDriveId, target, type).c_str());

    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;
    DWORD length = 0;

    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {};

    if (data == NULL)
    {
        SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL, "data == NULL");
        return FALSE;
    }

    // upstream quirk: upstream clears sizeof(ATA_IDENTIFY_DEVICE) — the first
    // 512 bytes of the 4096 byte union — here.  The caller pre-zeroes the whole
    // union, so no partial clear is performed.
    //  ::ZeroMemory(data, sizeof(ATA_IDENTIFY_DEVICE));

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL,
                          "! hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE");
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //::ZeroMemory(&sptwb, sizeof(SCSI_PASS_THROUGH_WITH_BUFFERS));

    sptwb.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    sptwb.Spt.PathId = 0;
    sptwb.Spt.TargetId = 0;
    sptwb.Spt.Lun = 0;
    sptwb.Spt.SenseInfoLength = 24;
    sptwb.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    sptwb.Spt.DataTransferLength = IDENTIFY_BUFFER_SIZE;
    sptwb.Spt.TimeOutValue = 2;
    sptwb.Spt.DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    sptwb.Spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);

    if (type == CMD_TYPE_SAT)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xA1;//ATA PASS THROUGH(12) OPERATION CODE(A1h)
        sptwb.Spt.Cdb[1] = (4 << 1) | 0; //MULTIPLE_COUNT=0,PROTOCOL=4(PIO Data-In),Reserved
        sptwb.Spt.Cdb[2] = (1 << 3) | (1 << 2) | 2;//OFF_LINE=0,CK_COND=0,Reserved=0,T_DIR=1(ToDevice),BYTE_BLOCK=1,T_LENGTH=2
        sptwb.Spt.Cdb[3] = 0;//FEATURES (7:0)
        sptwb.Spt.Cdb[4] = 1;//SECTOR_COUNT (7:0)
        sptwb.Spt.Cdb[5] = 0;//LBA_LOW (7:0)
        sptwb.Spt.Cdb[6] = 0;//LBA_MID (7:0)
        sptwb.Spt.Cdb[7] = 0;//LBA_HIGH (7:0)
        sptwb.Spt.Cdb[8] = target;
        sptwb.Spt.Cdb[9] = ID_CMD;//COMMAND
    }
    else if (type == CMD_TYPE_JMICRON)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xDF;
        sptwb.Spt.Cdb[1] = 0x10;
        sptwb.Spt.Cdb[2] = 0x00;
        sptwb.Spt.Cdb[3] = 0x02;
        sptwb.Spt.Cdb[4] = 0x00;
        sptwb.Spt.Cdb[5] = 0x00;
        sptwb.Spt.Cdb[6] = 0x01;
        sptwb.Spt.Cdb[7] = 0x00;
        sptwb.Spt.Cdb[8] = 0x00;
        sptwb.Spt.Cdb[9] = 0x00;
        sptwb.Spt.Cdb[10] = target;
        sptwb.Spt.Cdb[11] = 0xEC; // ID_CMD
    }
    else
    {
        SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL,
                          "COMMAND_TYPE_UNKNOWN");
        return FALSE;
    }

    length = static_cast<DWORD>(offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf))
             + sptwb.Spt.DataTransferLength;

    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_SCSI_PASS_THROUGH,
                             &sptwb, sizeof(SCSI_PASS_THROUGH),
                             &sptwb, length, &dwReturned, NULL);

    if (bRet == FALSE || dwReturned != length)
    {
        SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL,
                          TraceFmt("bRet == FALSE || dwReturned != length / Error Coce: %08X",
                                   GetLastError()).c_str());
        return FALSE;
    }

    // The only protection against a bridge that answers the pass-through with a
    // zeroed buffer (upstream also comments out the 0xA5 signature test).
    DWORD count = 0;
    for (int i = 0; i < 512; i++)
    {
        count += sptwb.DataBuf[i];
    }
    if (count == 0) // || sptwb.DataBuf[510] != 0xA5
    {
        SMART_TRACE_EVENT("DoIdentifyDeviceSat", SmartTraceCategory::READER_IOCTL, "count=0");

        return FALSE;
    }

    ::memcpy_s(data, sizeof(ATA_IDENTIFY_DEVICE), sptwb.DataBuf, sizeof(ATA_IDENTIFY_DEVICE));

    return TRUE;
}

// upstream AtaSmart.cpp:9609-9817 (CMD_TYPE_SAT / CMD_TYPE_JMICRON only)
BOOL CAtaSmart::GetSmartAttributeSat(INT physicalDriveId, BYTE target, DRIVE_INFO* asi)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;
    DWORD length;

    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {};

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //::ZeroMemory(&sptwb,sizeof(SCSI_PASS_THROUGH_WITH_BUFFERS));

    sptwb.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    sptwb.Spt.PathId = 0;
    sptwb.Spt.TargetId = 0;
    sptwb.Spt.Lun = 0;
    sptwb.Spt.SenseInfoLength = 24;
    sptwb.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    sptwb.Spt.DataTransferLength = READ_ATTRIBUTE_BUFFER_SIZE;
    sptwb.Spt.TimeOutValue = 2;
    sptwb.Spt.DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    sptwb.Spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);

    COMMAND_TYPE type = asi->CommandType;
    if (type == CMD_TYPE_SAT)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xA1;//ATA PASS THROUGH(12) OPERATION CODE(A1h)
        sptwb.Spt.Cdb[1] = (4 << 1) | 0; //MULTIPLE_COUNT=0,PROTOCOL=4(PIO Data-In),Reserved
        sptwb.Spt.Cdb[2] = (1 << 3) | (1 << 2) | 2;//OFF_LINE=0,CK_COND=0,Reserved=0,T_DIR=1(ToDevice),BYTE_BLOCK=1,T_LENGTH=2
        sptwb.Spt.Cdb[3] = READ_ATTRIBUTES;//FEATURES (7:0)
        sptwb.Spt.Cdb[4] = 1;//SECTOR_COUNT (7:0)
        sptwb.Spt.Cdb[5] = 1;//LBA_LOW (7:0)
        sptwb.Spt.Cdb[6] = SMART_CYL_LOW;//LBA_MID (7:0)
        sptwb.Spt.Cdb[7] = SMART_CYL_HI;//LBA_HIGH (7:0)
        sptwb.Spt.Cdb[8] = target;
        sptwb.Spt.Cdb[9] = SMART_CMD;//COMMAND
    }
    else if (type == CMD_TYPE_JMICRON)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xDF;
        sptwb.Spt.Cdb[1] = 0x10;
        sptwb.Spt.Cdb[2] = 0x00;
        sptwb.Spt.Cdb[3] = 0x02;
        sptwb.Spt.Cdb[4] = 0x00;
        sptwb.Spt.Cdb[5] = 0xD0;  // READ_ATTRIBUTES
        sptwb.Spt.Cdb[6] = 0x01;
        sptwb.Spt.Cdb[7] = 0x01;
        sptwb.Spt.Cdb[8] = 0x4F;  // SMART_CYL_LOW
        sptwb.Spt.Cdb[9] = 0xC2;  // SMART_CYL_HIGH
        sptwb.Spt.Cdb[10] = target;
        sptwb.Spt.Cdb[11] = 0xB0; // SMART_CMD
    }
    else
    {
        return FALSE;
    }

    length = static_cast<DWORD>(offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf))
             + sptwb.Spt.DataTransferLength;
    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_SCSI_PASS_THROUGH,
                             &sptwb, sizeof(SCSI_PASS_THROUGH),
                             &sptwb, length, &dwReturned, NULL);

    if (bRet == FALSE || dwReturned != length)
    {
        return FALSE;
    }

    DWORD count = 0;
    for (int i = 0; i < 512; i++)
    {
        count += sptwb.DataBuf[i];
    }
    if (count == 0)
    {
        return FALSE;
    }

    ::memcpy_s(&(asi->SmartReadData), 512, &(sptwb.DataBuf), 512);
    return FillSmartData(asi);
}

// upstream AtaSmart.cpp:9819-10017 (CMD_TYPE_SAT / CMD_TYPE_JMICRON only)
BOOL CAtaSmart::GetSmartThresholdSat(INT physicalDriveId, BYTE target, DRIVE_INFO* asi)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;
    DWORD length = 0;

    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {};

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //::ZeroMemory(&sptwb,sizeof(SCSI_PASS_THROUGH_WITH_BUFFERS));

    sptwb.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    sptwb.Spt.PathId = 0;
    sptwb.Spt.TargetId = 0;
    sptwb.Spt.Lun = 0;
    sptwb.Spt.SenseInfoLength = 24;
    sptwb.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    sptwb.Spt.DataTransferLength = READ_THRESHOLD_BUFFER_SIZE;
    sptwb.Spt.TimeOutValue = 2;
    sptwb.Spt.DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    sptwb.Spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);

    COMMAND_TYPE type = asi->CommandType;
    if (type == CMD_TYPE_SAT)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xA1; ////ATA PASS THROUGH(12) OPERATION CODE (A1h)
        sptwb.Spt.Cdb[1] = (4 << 1) | 0; //MULTIPLE_COUNT=0,PROTOCOL=4(PIO Data-In),Reserved
        sptwb.Spt.Cdb[2] = (1 << 3) | (1 << 2) | 2;//OFF_LINE=0,CK_COND=0,Reserved=0,T_DIR=1(ToDevice),BYTE_BLOCK=1,T_LENGTH=2
        sptwb.Spt.Cdb[3] = READ_THRESHOLDS;//FEATURES (7:0)
        sptwb.Spt.Cdb[4] = 1;//SECTOR_COUNT (7:0)
        sptwb.Spt.Cdb[5] = 1;//LBA_LOW (7:0)
        sptwb.Spt.Cdb[6] = SMART_CYL_LOW;//LBA_MID (7:0)
        sptwb.Spt.Cdb[7] = SMART_CYL_HI;//LBA_HIGH (7:0)
        sptwb.Spt.Cdb[8] = target;
        sptwb.Spt.Cdb[9] = SMART_CMD;//COMMAND
    }
    else if (type == CMD_TYPE_JMICRON)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xDF;
        sptwb.Spt.Cdb[1] = 0x10;
        sptwb.Spt.Cdb[2] = 0x00;
        sptwb.Spt.Cdb[3] = 0x02;
        sptwb.Spt.Cdb[4] = 0x00;
        sptwb.Spt.Cdb[5] = 0xD1;  // READ_THRESHOLD
        sptwb.Spt.Cdb[6] = 0x01;
        sptwb.Spt.Cdb[7] = 0x01;
        sptwb.Spt.Cdb[8] = 0x4F;  // SMART_CYL_LOW
        sptwb.Spt.Cdb[9] = 0xC2;  // SMART_CYL_HIGH
        sptwb.Spt.Cdb[10] = target;
        sptwb.Spt.Cdb[11] = 0xB0; // SMART_CMD
    }
    else
    {
        return FALSE;
    }

    length = static_cast<DWORD>(offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf))
             + sptwb.Spt.DataTransferLength;
    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_SCSI_PASS_THROUGH,
                             &sptwb, sizeof(SCSI_PASS_THROUGH),
                             &sptwb, length, &dwReturned, NULL);

    if (bRet == FALSE || dwReturned != length)
    {
        return FALSE;
    }

    ::memcpy_s(&(asi->SmartReadThreshold), 512, &(sptwb.DataBuf), 512);
    return FillSmartThreshold(asi);
}

// upstream AtaSmart.cpp:10019-10204 (CMD_TYPE_SAT / CMD_TYPE_JMICRON only)
BOOL CAtaSmart::ControlSmartStatusSat(INT physicalDriveId, BYTE target, BYTE command,
                                      COMMAND_TYPE type)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {};

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //::ZeroMemory(&sptwb,sizeof(SCSI_PASS_THROUGH_WITH_BUFFERS));

    sptwb.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    sptwb.Spt.PathId = 0;
    sptwb.Spt.TargetId = 0;
    sptwb.Spt.Lun = 0;
    sptwb.Spt.SenseInfoLength = 24;
    sptwb.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    sptwb.Spt.DataTransferLength = 0;
    sptwb.Spt.TimeOutValue = 2;
    sptwb.Spt.DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    sptwb.Spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);

    if (type == CMD_TYPE_SAT)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xA1; //ATA PASS THROUGH (12) OPERATION CODE (A1h)
        sptwb.Spt.Cdb[1] = (3 << 1) | 0; //MULTIPLE_COUNT=0,PROTOCOL=3(Non-Data),Reserved
        sptwb.Spt.Cdb[2] = (1 << 3) | (1 << 2) | 2;//OFF_LINE=0,CK_COND=0,Reserved=0,T_DIR=1(ToDevice),BYTE_BLOCK=1,T_LENGTH=2
        sptwb.Spt.Cdb[3] = command;//FEATURES (7:0)
        sptwb.Spt.Cdb[4] = 0;//SECTOR_COUNT (7:0)
        sptwb.Spt.Cdb[5] = 1;//LBA_LOW (7:0)
        sptwb.Spt.Cdb[6] = SMART_CYL_LOW;//LBA_MID (7:0)
        sptwb.Spt.Cdb[7] = SMART_CYL_HI;//LBA_HIGH (7:0)
        sptwb.Spt.Cdb[8] = target;
        sptwb.Spt.Cdb[9] = SMART_CMD;//COMMAND
    }
    else if (type == CMD_TYPE_JMICRON)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0] = 0xDF;
        sptwb.Spt.Cdb[1] = 0x10;
        sptwb.Spt.Cdb[2] = 0x00;
        sptwb.Spt.Cdb[3] = 0x02;
        sptwb.Spt.Cdb[4] = 0x00;
        sptwb.Spt.Cdb[5] = command;
        sptwb.Spt.Cdb[6] = 0x01;
        sptwb.Spt.Cdb[7] = 0x01;
        sptwb.Spt.Cdb[8] = 0x4F;  // SMART_CYL_LOW
        sptwb.Spt.Cdb[9] = 0xC2;  // SMART_CYL_HIGH
        sptwb.Spt.Cdb[10] = target;
        sptwb.Spt.Cdb[11] = 0xB0; // SMART_CMD
    }
    else
    {
        return FALSE;
    }

    DWORD length = static_cast<DWORD>(offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf))
                   + sptwb.Spt.DataTransferLength;
    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_SCSI_PASS_THROUGH,
                             &sptwb, sizeof(SCSI_PASS_THROUGH),
                             &sptwb, length, &dwReturned, NULL);

    return bRet;
}

// upstream AtaSmart.cpp:10206-10396 (CMD_TYPE_SAT / CMD_TYPE_JMICRON only)
BOOL CAtaSmart::SendAtaCommandSat(INT physicalDriveId, BYTE target, BYTE main, BYTE sub,
                                  BYTE param, COMMAND_TYPE type)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    SCSI_PASS_THROUGH_WITH_BUFFERS sptwb = {};

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    //::ZeroMemory(&sptwb,sizeof(SCSI_PASS_THROUGH_WITH_BUFFERS));

    sptwb.Spt.Length = sizeof(SCSI_PASS_THROUGH);
    sptwb.Spt.PathId = 0;
    sptwb.Spt.TargetId = 0;
    sptwb.Spt.Lun = 0;
    sptwb.Spt.SenseInfoLength = 24;
    sptwb.Spt.DataIn = SCSI_IOCTL_DATA_IN;
    sptwb.Spt.DataTransferLength = 0;
    sptwb.Spt.TimeOutValue = 2;
    sptwb.Spt.DataBufferOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf);
    sptwb.Spt.SenseInfoOffset = offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, SenseBuf);
    if (type == CMD_TYPE_SAT)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0]  = 0xA1; //ATA PASS THROUGH (12) OPERATION CODE (A1h)
        sptwb.Spt.Cdb[1]  = (3 << 1) | 0; //MULTIPLE_COUNT=0,PROTOCOL=3(Non-Data),Reserved
        sptwb.Spt.Cdb[2]  = (1 << 3) | (1 << 2) | 2;//OFF_LINE=0,CK_COND=0,Reserved=0,T_DIR=1(ToDevice),BYTE_BLOCK=1,T_LENGTH=2
        sptwb.Spt.Cdb[3]  = sub;        //FEATURES (7:0)
        sptwb.Spt.Cdb[4]  = param;      //SECTOR_COUNT (7:0)
        sptwb.Spt.Cdb[5]  = 0x00;       //LBA_LOW (7:0)
        sptwb.Spt.Cdb[6]  = 0x00;       //LBA_MID (7:0)
        sptwb.Spt.Cdb[7]  = 0x00;       //LBA_HIGH (7:0)
        sptwb.Spt.Cdb[8]  = target;     //DEVICE_HEAD
        sptwb.Spt.Cdb[9]  = main;       //COMMAND
        sptwb.Spt.Cdb[10] = 0x00;
        sptwb.Spt.Cdb[11] = 0x00;
    }
    else if (type == CMD_TYPE_JMICRON)
    {
        sptwb.Spt.CdbLength = 12;
        sptwb.Spt.Cdb[0]  = 0xDF;
        sptwb.Spt.Cdb[1]  = 0x10;
        sptwb.Spt.Cdb[2]  = 0x00;
        sptwb.Spt.Cdb[3]  = 0x02;
        sptwb.Spt.Cdb[4]  = 0x00;
        sptwb.Spt.Cdb[5]  = sub;
        sptwb.Spt.Cdb[6]  = param;
        sptwb.Spt.Cdb[7]  = 0x00;
        sptwb.Spt.Cdb[8]  = 0x00;
        sptwb.Spt.Cdb[9]  = 0x00;
        sptwb.Spt.Cdb[10] = target;
        sptwb.Spt.Cdb[11] = main;
    }
    else
    {
        return FALSE;
    }

    DWORD length = static_cast<DWORD>(offsetof(SCSI_PASS_THROUGH_WITH_BUFFERS, DataBuf))
                   + sptwb.Spt.DataTransferLength;
    bRet = ::DeviceIoControl(hIoCtrl, IOCTL_SCSI_PASS_THROUGH,
                             &sptwb, sizeof(SCSI_PASS_THROUGH),
                             &sptwb, length, &dwReturned, NULL);

    return bRet;
}

/*---------------------------------------------------------------------------*/
//  Legacy ATA / SMART IOCTL fallback — DFP_RECEIVE_DRIVE_DATA
/*---------------------------------------------------------------------------*/

// The DFP_RECEIVE_DRIVE_DATA exchange upstream repeats in DoIdentifyDevicePd
// (AtaSmart.cpp 7013-7052), GetSmartAttributePd (7080-7108) and
// GetSmartThresholdPd (7135-7163) — plus the SENDCMDINPARAMS register set that
// upstream's ControlSmartStatusPd uses for DFP_SEND_DRIVE_COMMAND — factored
// out into one helper.
BOOL CAtaSmart::SendSmartRcvDriveDataSi(INT physicalDriveId, BYTE subCommand,
                                        BYTE* data, DWORD dataSize)
{
    BOOL  bRet = FALSE;
    DWORD dwReturned = 0;

    IDENTIFY_DEVICE_OUTDATA sendCmdOutParam = {};
    SENDCMDINPARAMS         sendCmd = {};

    // Upstream's identify path (DoIdentifyDevicePd) rejects a NULL buffer
    // before doing any I/O; the check is kept here so the factored-out helper
    // preserves that behaviour for every sub-command.
    if (data == NULL)
    {
        return FALSE;
    }

    HANDLE hIoCtrl = GetIoCtrlHandle(physicalDriveId);
    if (!hIoCtrl || hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }
    HandleCloser closer(hIoCtrl);

    sendCmd.irDriveRegs.bFeaturesReg     = subCommand;
    sendCmd.irDriveRegs.bSectorCountReg  = 1;
    sendCmd.irDriveRegs.bSectorNumberReg = 1;
    sendCmd.irDriveRegs.bCylLowReg       = SMART_CYL_LOW;
    sendCmd.irDriveRegs.bCylHighReg      = SMART_CYL_HI;
    sendCmd.irDriveRegs.bDriveHeadReg    = 0xA0;
    sendCmd.irDriveRegs.bCommandReg      = (subCommand == 0x00) ? ID_CMD : SMART_CMD;
    sendCmd.cBufferSize                  = dataSize;

    SMART_TRACE_EVENT("SendSmartRcvDriveDataSi", SmartTraceCategory::READER_IOCTL,
                      TraceFmt("DFP_RECEIVE_DRIVE_DATA sub=0x%02X size=%u",
                               subCommand, dataSize).c_str());

    bRet = ::DeviceIoControl(hIoCtrl, DFP_RECEIVE_DRIVE_DATA,
                             &sendCmd, sizeof(SENDCMDINPARAMS),
                             &sendCmdOutParam, sizeof(IDENTIFY_DEVICE_OUTDATA),
                             &dwReturned, NULL);

    if (bRet == FALSE || dwReturned != sizeof(IDENTIFY_DEVICE_OUTDATA))
    {
        return FALSE;
    }

    ::memcpy_s(data, dataSize, sendCmdOutParam.SendCmdOutParam.bBuffer, dataSize);

    return TRUE;
}

// upstream AtaSmart.cpp:10502-10535 — interface only.  Upstream sends a
// CMD_IDE SRB_IO_CONTROL miniport request there, which is out of scope (see
// the file banner); the drive exchange itself is the factored-out helper above.
BOOL CAtaSmart::DoIdentifyDeviceSi(INT physicalDriveId, BYTE target, IDENTIFY_DEVICE* data)
{
    // SendSmartRcvDriveDataSi()'s frozen signature carries no target, so the
    // drive/head value cannot be forwarded to bDriveHeadReg; the helper uses
    // the 0xA0 (master) value that every caller of the identify path passes.
    (void)target;

    return SendSmartRcvDriveDataSi(physicalDriveId, 0x00, (BYTE*)data,
                                   sizeof(ATA_IDENTIFY_DEVICE));
}

// upstream AtaSmart.cpp:10537-10560 — the IOCTL_STORAGE_PREDICT_FAILURE
// alternate and the GetSmartAttributeWmi() fallback are out of scope.
BOOL CAtaSmart::GetSmartAttributeSi(INT physicalDriveId, DRIVE_INFO* asi)
{
    if (!SendSmartRcvDriveDataSi(physicalDriveId, READ_ATTRIBUTES,
                                 asi->SmartReadData, sizeof(asi->SmartReadData)))
    {
        return FALSE;
    }

    return FillSmartData(asi);
}

// upstream AtaSmart.cpp:10562-10564 — GetSmartThresholdWmi() is out of scope.
BOOL CAtaSmart::GetSmartThresholdSi(INT physicalDriveId, DRIVE_INFO* asi)
{
    if (!SendSmartRcvDriveDataSi(physicalDriveId, READ_THRESHOLDS,
                                 asi->SmartReadThreshold, sizeof(asi->SmartReadThreshold)))
    {
        return FALSE;
    }

    return FillSmartThreshold(asi);
}

/*---------------------------------------------------------------------------*/
//  NVMe — Storage Protocol Specific Query (Windows 10+)
//
//  Adaptation: the property id is decided at call time rather than being fixed
//  to StorageAdapterProtocolSpecificProperty as upstream has it — see
//  StorageQueryIoControl() below.  Query.QueryType stays PropertyStandardQuery,
//  matching upstream.
/*---------------------------------------------------------------------------*/

// upstream AtaSmart.cpp:8958-9008
BOOL CAtaSmart::DoIdentifyDeviceNVMeStorageQuery(INT physicalDriveId, IDENTIFY_DEVICE* data,
                                                 DWORD* diskSize)
{
    const std::wstring path = detail::FormatW(L"\\\\.\\PhysicalDrive%d", physicalDriveId);

    HANDLE hIoCtrl = ::CreateFile(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    HandleCloser closer(hIoCtrl);

    StorageQuery::TStorageQueryWithBuffer nptwb;
    BOOL bRet = 0;
    ::ZeroMemory(&nptwb, sizeof(nptwb));

    nptwb.ProtocolSpecific.ProtocolType = StorageQuery::ProtocolTypeNvme;
    nptwb.ProtocolSpecific.DataType = StorageQuery::NVMeDataTypeIdentify;
    nptwb.ProtocolSpecific.ProtocolDataOffset = sizeof(StorageQuery::STORAGE_PROTOCOL_SPECIFIC_DATA_CDI);
    nptwb.ProtocolSpecific.ProtocolDataLength = 4096;
    nptwb.ProtocolSpecific.ProtocolDataRequestValue = 0;
    nptwb.ProtocolSpecific.ProtocolDataRequestSubValue = 1;
    nptwb.Query.QueryType = StorageQuery::PropertyStandardQuery;
    DWORD dwReturned = 0;

    // upstream quirk: the handle is never validated before this call
    // (CreateFile returning INVALID_HANDLE_VALUE simply makes the IOCTL fail).
    bRet = StorageQueryIoControl(hIoCtrl, &nptwb, &dwReturned);

    if (bRet)
    {
        ULONGLONG totalLBA = B8toB64le(&nptwb.Buffer[0]);
        int sectorSize = 1 << nptwb.Buffer[130];
        // upstream quirk: diskSize is produced by this first call, while the
        // function's return value comes from the second call below.
        *diskSize = (DWORD)(totalLBA * sectorSize / 1000 / 1000);
    }

    ::ZeroMemory(&nptwb, sizeof(nptwb));
    nptwb.ProtocolSpecific.ProtocolType = StorageQuery::ProtocolTypeNvme;
    nptwb.ProtocolSpecific.DataType = StorageQuery::NVMeDataTypeIdentify;
    nptwb.ProtocolSpecific.ProtocolDataOffset = sizeof(StorageQuery::STORAGE_PROTOCOL_SPECIFIC_DATA_CDI);
    nptwb.ProtocolSpecific.ProtocolDataLength = 4096;
    nptwb.Query.QueryType = StorageQuery::PropertyStandardQuery;
    nptwb.ProtocolSpecific.ProtocolDataRequestValue = 1; /*NVME_IDENTIFY_CNS_CONTROLLER*/
    nptwb.ProtocolSpecific.ProtocolDataRequestSubValue = 0;
    dwReturned = 0;

    bRet = StorageQueryIoControl(hIoCtrl, &nptwb, &dwReturned);

    ::memcpy_s(data, sizeof(NVME_IDENTIFY_DEVICE), nptwb.Buffer, sizeof(NVME_IDENTIFY_DEVICE));

    return bRet;
}

// upstream AtaSmart.cpp:9010-9052
BOOL CAtaSmart::GetSmartAttributeNVMeStorageQuery(INT physicalDriveId, DRIVE_INFO* asi)
{
    const std::wstring path = detail::FormatW(L"\\\\.\\PhysicalDrive%d", physicalDriveId);

    HANDLE hIoCtrl = ::CreateFile(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    HandleCloser closer(hIoCtrl);
    BOOL bRet = 0;

    StorageQuery::TStorageQueryWithBuffer nptwb = {};
    //::ZeroMemory(&nptwb, sizeof(nptwb));

    nptwb.ProtocolSpecific.ProtocolType = StorageQuery::ProtocolTypeNvme;
    nptwb.ProtocolSpecific.DataType = StorageQuery::NVMeDataTypeLogPage;
    nptwb.ProtocolSpecific.ProtocolDataRequestValue = 2; // SMART Health Information
    nptwb.ProtocolSpecific.ProtocolDataRequestSubValue = 0x00000000;
    nptwb.ProtocolSpecific.ProtocolDataOffset = sizeof(StorageQuery::STORAGE_PROTOCOL_SPECIFIC_DATA_CDI);
    nptwb.ProtocolSpecific.ProtocolDataLength = 4096;
    nptwb.Query.QueryType = StorageQuery::PropertyStandardQuery;
    DWORD dwReturned = 0;

    bRet = StorageQueryIoControl(hIoCtrl, &nptwb, &dwReturned);
    if (!bRet)
    {
        // upstream quirk: the retry exists because the two NVMe driver
        // generations disagree about the meaning of the sub-value.
        nptwb.ProtocolSpecific.ProtocolDataRequestSubValue = 0xFFFFFFFF;
        bRet = StorageQueryIoControl(hIoCtrl, &nptwb, &dwReturned);
    }

    ::memcpy_s(&(asi->SmartReadData), 512, nptwb.Buffer, 512);

    BYTE NvmeIdentifyControllerData[4096] = {};
    if (GetNvMeIdentifyControllerData(physicalDriveId, NvmeIdentifyControllerData))
    {
        asi->IsNvmeThresholdSupported = IsNVMeTemperatureThresholdDefined(NvmeIdentifyControllerData);
        asi->IsNvmeThermalManagementSupported = IsNVMeThermalManagementTemperatureDefined(NvmeIdentifyControllerData);
    }
    return bRet;
}

// upstream AtaSmart.cpp:9054-9090
BOOL CAtaSmart::GetNvMeIdentifyControllerData(INT physicalDriveId, BYTE* outBuffer)
{
    const std::wstring path = detail::FormatW(L"\\\\.\\PhysicalDrive%d", physicalDriveId);

    HANDLE hIoCtrl = ::CreateFile(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    HandleCloser closer(hIoCtrl);
    StorageQuery::TStorageQueryWithBuffer nptwb = {};
    BOOL bRet = 0;
    ::ZeroMemory(&nptwb, sizeof(nptwb));

    if (hIoCtrl == INVALID_HANDLE_VALUE)
    {
        return FALSE;
    }

    nptwb.ProtocolSpecific.ProtocolType = StorageQuery::ProtocolTypeNvme;
    nptwb.ProtocolSpecific.DataType = StorageQuery::NVMeDataTypeIdentify;
    nptwb.ProtocolSpecific.ProtocolDataOffset = sizeof(StorageQuery::STORAGE_PROTOCOL_SPECIFIC_DATA_CDI);
    nptwb.ProtocolSpecific.ProtocolDataLength = 4096;
    nptwb.Query.QueryType = StorageQuery::PropertyStandardQuery;
    nptwb.ProtocolSpecific.ProtocolDataRequestValue = 1; /*NVME_IDENTIFY_CNS_CONTROLLER*/
    nptwb.ProtocolSpecific.ProtocolDataRequestSubValue = 0;

    DWORD dwReturned = 0;

    bRet = StorageQueryIoControl(hIoCtrl, &nptwb, &dwReturned);

    ::memcpy_s(outBuffer, sizeof(NVME_IDENTIFY_DEVICE), nptwb.Buffer, sizeof(NVME_IDENTIFY_DEVICE));

    return bRet;
}

// upstream AtaSmart.cpp:9092-9098
BOOL CAtaSmart::IsNVMeTemperatureThresholdDefined(BYTE* identifyControllerData)
{
    USHORT WCTemp = B8toB16le_ptr(&identifyControllerData[266]);
    USHORT CCTemp = B8toB16le_ptr(&identifyControllerData[268]);

    return (WCTemp != 0 || CCTemp != 0);
}

// upstream AtaSmart.cpp:9100-9106
BOOL CAtaSmart::IsNVMeThermalManagementTemperatureDefined(BYTE* identifyControllerData)
{
    USHORT minTMT = B8toB16le_ptr(&identifyControllerData[324]);
    USHORT maxTMT = B8toB16le_ptr(&identifyControllerData[326]);

    return (minTMT != 0 || maxTMT != 0);
}

} // namespace cdi
