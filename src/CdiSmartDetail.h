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
//  CdiSmartDetail — shared internals of the CrystalDiskInfo S.M.A.R.T. port
//
//  The port is spread over several translation units (the upstream
//  AtaSmart.cpp is a single 13 570 line file; splitting it keeps each unit
//  reviewable).  Everything that more than one of those units needs lives
//  here:
//
//    * little endian readers over the 6-byte SMART raw field
//    * std::wstring helpers replacing the MFC CString vocabulary upstream
//      uses (MakeUpper / Find / Right / Format)
//    * the String tables CrystalDiskInfo keeps as file statics in
//      AtaSmart.h (commandTypeString / ssdVendorString / deviceFormFactorString)
//    * the FlagLife* / NandWritesUnit side channel (see below)
//    * time unit detection, which both AddDisk() and CheckDiskStatus() need
//
//  Nothing here is part of the public surface of the module; callers outside
//  src/CdiSmart*.cpp should use CdiSmart.h and CdiAttributeName.h only.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmart.h"

#include <string>
#include <map>
#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace cdi {
namespace detail {

// ── Little endian byte readers ────────────────────────────────────────
//
// CdiSmart.h only declares the pointer forms.  Upstream (Priscilla/
// UtilityFx.h) also has array-reference forms and the signed B8toINTle,
// which the HOST_READS_WRITES_GB branches depend on: those fields carry a
// count that can have bit 31 set, and reading them unsigned changes the
// sign of HostReads/HostWrites.

inline ULONGLONG B8toB64lePtr(const BYTE* v) {
    ULONGLONG u64;
    ::memcpy(&u64, v, sizeof(u64));
    return u64;
}

inline DWORD B8toB32lePtr(const BYTE* v) {
    return static_cast<DWORD>(v[0]) | (static_cast<DWORD>(v[1]) << 8) |
           (static_cast<DWORD>(v[2]) << 16) | (static_cast<DWORD>(v[3]) << 24);
}

inline INT B8toINTlePtr(const BYTE* v) {
    return static_cast<INT>(B8toB32lePtr(v));
}

inline SHORT B8toSHORTlePtr(const BYTE* v) {
    return static_cast<SHORT>(static_cast<USHORT>(v[0]) | (static_cast<USHORT>(v[1]) << 8));
}

inline ULONGLONG B8toB64le(const BYTE (&v)[6]) {
    return static_cast<ULONGLONG>(v[0]) |
           (static_cast<ULONGLONG>(v[1]) << 8) |
           (static_cast<ULONGLONG>(v[2]) << 16) |
           (static_cast<ULONGLONG>(v[3]) << 24) |
           (static_cast<ULONGLONG>(v[4]) << 32) |
           (static_cast<ULONGLONG>(v[5]) << 40);
}

inline DWORD B8toB32le(const BYTE (&v)[6]) {
    return static_cast<DWORD>(v[0]) | (static_cast<DWORD>(v[1]) << 8) |
           (static_cast<DWORD>(v[2]) << 16) | (static_cast<DWORD>(v[3]) << 24);
}

inline INT B8toINTle(const BYTE (&v)[6]) {
    return static_cast<INT>(B8toB32le(v));
}

inline USHORT B8toB16le(const BYTE (&v)[6]) {
    return static_cast<USHORT>(static_cast<USHORT>(v[0]) | (static_cast<USHORT>(v[1]) << 8));
}

// ── std::wstring helpers ──────────────────────────────────────────────
//
// These replace the CString vocabulary used upstream.  Semantics follow
// CString exactly where the ported logic depends on it:
//   CString::Find(x) == 0    -> StartsWith
//   CString::Find(x) != -1   -> Contains
//   CString::Find(x) > 0     -> ContainsAtOrAfter1
//   CString::Right(3)        -> Right

inline std::wstring ToUpperCopy(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(::towupper(c));
    return s;
}

inline bool StartsWith(const std::wstring& s, const wchar_t* prefix) {
    if (!prefix) return false;
    const size_t n = wcslen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

inline bool EndsWith(const std::wstring& s, const wchar_t* suffix) {
    if (!suffix) return false;
    const size_t n = wcslen(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

inline bool Contains(const std::wstring& s, const wchar_t* sub) {
    if (!sub) return false;
    return s.find(sub) != std::wstring::npos;
}

// CString::Find(sub) > 0 — i.e. found, but strictly after position 0.
inline bool ContainsAfter0(const std::wstring& s, const wchar_t* sub) {
    if (!sub) return false;
    const size_t pos = s.find(sub);
    return pos != std::wstring::npos && pos > 0;
}

inline std::wstring Right(const std::wstring& s, size_t n) {
    return s.size() <= n ? s : s.substr(s.size() - n);
}

inline std::wstring FormatW(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

// ── String tables (CrystalDiskInfo AtaSmart.h file statics) ───────────
//
// Exposed as extern because AddDisk() (CdiSmart.cpp) and CheckSsdSupport()
// (CdiSmartSsd.cpp) both index them.  Definitions live in CdiSmart.cpp.

// Indexed by COMMAND_TYPE — only the 8 values present in CdiSmart.h.
extern const wchar_t* const kCommandTypeString[8];

// Indexed by VENDOR_ID, 51 entries, verbatim from upstream AtaSmart.h.
// CheckSsdSupport() writes ssdVendorString[DiskVendorId] into
// DRIVE_INFO::SsdVendorString.
extern const wchar_t* const kSsdVendorString[51];
constexpr int SSD_VENDOR_STRING_COUNT = 51;

// Indexed by (IDENTIFY word 168 >> 12) & 0x0F, verbatim from upstream.
extern const wchar_t* const kDeviceFormFactorString[6];

// CrystalDiskInfo AddDisk(): deviceFormFactorString[(w168 >> 12) & 0xF]
// clamped into range, so the caller never indexes out of bounds.
std::wstring DeviceFormFactorString(WORD word168);

// ── Transfer mode enum (CrystalDiskInfo AtaSmart.h) ───────────────────
//
// The *numeric ordering is load-bearing*: GetTimeUnitType() compares
// transferMode >= TRANSFER_MODE_SATA_300.  Copy verbatim.
enum TransferMode {
    TRANSFER_MODE_UNKNOWN = 0,
    TRANSFER_MODE_PIO,
    TRANSFER_MODE_PIO_DMA,
    TRANSFER_MODE_ULTRA_DMA_16,
    TRANSFER_MODE_ULTRA_DMA_25,
    TRANSFER_MODE_ULTRA_DMA_33,
    TRANSFER_MODE_ULTRA_DMA_44,
    TRANSFER_MODE_ULTRA_DMA_66,
    TRANSFER_MODE_ULTRA_DMA_100,
    TRANSFER_MODE_ULTRA_DMA_133,
    TRANSFER_MODE_SATA_150,
    TRANSFER_MODE_SATA_300,
    TRANSFER_MODE_SATA_600
};

// ── FlagLife* / NandWritesUnit side channel ───────────────────────────
//
// CrystalDiskInfo keeps nine booleans on ATA_SMART_INFO that its IsSsdXxx
// helpers set and that CheckSsdSupport(), FillSmartData() and
// CheckDiskStatus() read:
//
//   FlagLifeNoReport, FlagLifeRawValue, FlagLifeRawValueIncrement,
//   FlagLifeSanDiskUsbMemory, FlagLifeSanDisk0_1, FlagLifeSanDisk1,
//   FlagLifeSanDiskLenovo, FlagLifeSanDiskCloud, NandWritesUnit
//
// DRIVE_INFO deliberately does not carry them (they are display policy, not
// disk state).  They live in a side table instead, keyed on Model + Serial
// Number rather than on the DRIVE_INFO address: AddDisk() builds a local
// DRIVE_INFO and only then copies it into m_vars, and m_vars indices shift
// when a duplicate drive is removed, so neither a pointer nor an index is a
// stable key.

struct SsdLifeCtx {
    BOOL FlagLifeNoReport{};
    BOOL FlagLifeRawValue{};
    BOOL FlagLifeRawValueIncrement{};
    BOOL FlagLifeSanDiskUsbMemory{};
    BOOL FlagLifeSanDisk0_1{};
    BOOL FlagLifeSanDisk1{};
    BOOL FlagLifeSanDiskLenovo{};
    BOOL FlagLifeSanDiskCloud{};
    BOOL NandWrites1MB{};        // NandWritesUnit == NAND_WRITES_1MB
};

std::wstring LifeCtxKey(const DRIVE_INFO& asi);

// Get-or-create — used by CheckSsdSupport() while classifying a drive.
SsdLifeCtx& LifeCtx(DRIVE_INFO& asi);

// Read-only lookup; returns a default-constructed context when the drive was
// never classified.  Returns by value so callers cannot dangle.
SsdLifeCtx LifeCtxOf(const DRIVE_INFO& asi);

// Drops every stored context.  Called from CAtaSmart::Init() so a re-init
// does not accumulate stale entries.
void ClearLifeCtx();

// ── Time unit detection (CrystalDiskInfo CAtaSmart::GetTimeUnitType) ──

DWORD GetTimeUnitType(const std::wstring& model, const std::wstring& firmware,
                      DWORD major, DWORD transferMode);

// CrystalDiskInfo CAtaSmart::GetAtaMajorVersion — also fills the
// "ACS-3" / "ATA8-ACS" / "ATA/ATAPI-7" style version string.
DWORD GetAtaMajorVersion(WORD w80, std::wstring& majorVersion);

// ── Attribute cross-check ─────────────────────────────────────────────

// CrystalDiskInfo CAtaSmart::CheckSmartAttributeCorrect.
// Compares the *set* of attribute ids only — two reads of a live drive
// differ in RawValue by design, so values must never be compared here.
BOOL CheckSmartAttributeCorrect(const DRIVE_INFO* asi1, const DRIVE_INFO* asi2);

// ── Vendor predicates shared across units ─────────────────────────────

// CrystalDiskInfo CAtaSmart::IsSamsungEnterpriseModel — used by
// FillSmartData(), CheckSsdSupport() and CheckDiskStatus().
BOOL IsSamsungEnterpriseModel(const std::wstring& model);

// ── Misc helpers ──────────────────────────────────────────────────────

// CrystalDiskInfo CAtaSmart::CheckAsciiStringError.  Note it *mutates*:
// control characters 0x01..0x1F are overwritten with a space.
BOOL CheckAsciiStringError(char* str, DWORD length);

// CrystalDiskInfo CAtaSmart::WakeUp — reads the first sector with read-only
// access so a spun-down disk answers the following IDENTIFY.
void WakeUpDisk(INT physicalDriveId);

// True on Windows 10 or later.  Uses RtlGetVersion rather than
// VerifyVersionInfoW, which is manifest-shimmed and would report Windows 8
// for an application without a supportedOS manifest entry.
BOOL IsWindows10OrGreater();

} // namespace detail
} // namespace cdi
