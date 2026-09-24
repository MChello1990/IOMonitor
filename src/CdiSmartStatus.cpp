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
//  CdiSmartStatus — health evaluation + time-unit detection
//
//  Ports the following functions of CrystalDiskInfo
//  <https://github.com/hiyohiyo/CrystalDiskInfo> (Copyright (c) hiyohiyo,
//  MIT License).  Line numbers are from the upstream AtaSmart.cpp this
//  port was taken from.
//
//    CAtaSmart::CheckDiskStatus        AtaSmart.cpp:12522-12830
//    CAtaSmart::GetPowerOnHours        AtaSmart.cpp:12862-12891
//    CAtaSmart::MeasuredTimeUnit       AtaSmart.cpp:654-742
//    CAtaSmart::GetTimeUnitType        AtaSmart.cpp:12990-13072
//    CAtaSmart::GetAtaMajorVersion     AtaSmart.cpp:13074-13118
//    CAtaSmart::CheckAsciiStringError  AtaSmart.cpp:12843-12860
//    CAtaSmart::WakeUp                 AtaSmart.cpp:6412-6433  (WakeUpDisk)
//
//  Not ported:
//    CAtaSmart::IsWindowsVersionOrGreaterFx / IsWindows10OrGreater
//      Upstream wraps VerifyVersionInfoW, which is manifest-shimmed: a
//      process without a <supportedOS> entry in its manifest is told it
//      is running Windows 8 no matter what is really installed.  The
//      port resolves RtlGetVersion from ntdll.dll at runtime instead.
//
//  ── Substitutions for fields DRIVE_INFO does not carry ───────────────
//
//  PowerOnStartRawValue
//      Upstream stores the delta baseline on ATA_SMART_INFO
//      (AtaSmart.h:1860) and fills it in AddDisk()
//      (AtaSmart.cpp:3716, set to -1 when the SMART read failed).
//      DRIVE_INFO has no such member, so the baseline lives in the
//      file-local `s_powerOnStartRawValue` map in this unit, keyed on
//      PhysicalDriveId — not on the m_vars index, which shifts when
//      Init() drops a duplicate drive.  AddDisk() cannot be hooked from
//      here, so a drive's baseline is captured the first time
//      MeasuredTimeUnit() sees it.
//
//  MeasuredGetTickCount
//      Upstream member (AtaSmart.h:1962) set at the end of
//      CAtaSmart::Init() (AtaSmart.cpp:2426).  Init() lives in another
//      translation unit, so the 125..155 s window opens on the first
//      MeasuredTimeUnit() call instead (which also captures the
//      per-drive baselines above, so the two stay in step).
//
//  FlagLifeNoReport / FlagLifeRawValue / FlagLifeRawValueIncrement /
//  FlagLifeSanDiskUsbMemory / FlagLifeSanDisk0_1 / FlagLifeSanDisk1 /
//  FlagLifeSanDiskLenovo / FlagLifeSanDiskCloud
//      Upstream members of ATA_SMART_INFO set by CheckSsdSupport() and
//      read by CheckDiskStatus().  The port keeps them in the
//      cdi::detail::SsdLifeCtx side table (CdiSmartDetail.h), read here
//      through cdi::detail::LifeCtxOf().  The local is named `lifeCtx`
//      and not `life`, because every branch below also declares
//      upstream's local `int life` and reads the flags through it.
//
//  IsRawValues7 / IsRawValues8
//      Upstream convenience booleans set from the model + firmware
//      fingerprint (AtaSmart.cpp:4209-4256).  Reconstructed here from
//      SmartKeyName, which CheckSsdSupport() fills with the matching
//      fingerprint string.  Deliberately not derived from DiskVendorId.
//
//  IsMaxtorMinute
//      Upstream MeasuredTimeUnit() also sets this (AtaSmart.cpp:719/724);
//      DRIVE_INFO has no such field and no ported code reads it (it only
//      drives a DiskInfoDlg label), so the write is dropped.  The
//      MeasuredTimeUnitType assignment it accompanies is kept.
//
//  DRIVE_INFO::Life write
//      Upstream CheckDiskStatus() assigns `vars[i].Life` in the
//      SanDisk LENOVO/DELL/HP branch (AtaSmart.cpp:12772).  The frozen
//      signature takes `const DRIVE_INFO&`, so that store is dropped
//      here; it cannot affect the return value, because the only reader
//      of Life inside CheckDiskStatus() is the SSD_VENDOR_NVME pre-block
//      and the two vendor ids are mutually exclusive.  Life is seeded by
//      the NVMe path / another unit.
//
//  Return type
//      Upstream MeasuredTimeUnit() returns BOOL; CdiSmart.h declares it
//      `void`, so the two `return FALSE` paths and the final `return
//      TRUE` become plain returns.  No caller reads the value.
// ═══════════════════════════════════════════════════════════════════════

#include "CdiSmartDetail.h"
#include "SmartDebug.h"

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <map>
#include <string>

namespace {

// ── File-local state used by CAtaSmart::MeasuredTimeUnit() ────────────
// See the PowerOnStartRawValue / MeasuredGetTickCount notes above.
ULONGLONG          s_measuredGetTickCount = 0;      // window opens here
BOOL               s_measuredWindowOpened = FALSE;  // window seeded yet?
std::map<INT, INT> s_powerOnStartRawValue;          // key: PhysicalDriveId

// Upstream CheckDiskStatus() has no DebugPrint() call at all; the port
// adds a single trace event per exit so the health computation is visible
// in the SMART debug window.  This helper keeps the control flow of the
// upstream tail byte-for-byte (only the `return` expressions are wrapped).
DWORD TraceAndReturn(const char* text, DWORD status)
{
    SMART_TRACE_EVENT("CheckDiskStatus", SmartTraceCategory::HEALTH_COMPUTE, text);
    return status;
}

} // anonymous namespace

namespace cdi {

// ═══════════════════════════════════════════════════════════════════════
//  CAtaSmart::CheckDiskStatus — CrystalDiskInfo AtaSmart.cpp:12522-12830
//
//  Upstream signature: DWORD CAtaSmart::CheckDiskStatus(DWORD i)
//  Every `vars[i].X` access became `asi.X`; the `vars.GetCount() == 0`
//  guard was dropped (the caller owns the reference, and it cannot be
//  dangling by construction).
//
//  The Threshold05 / ThresholdC5 / ThresholdC6 / ThresholdFF comparisons
//  below are written exactly as upstream writes them.  DRIVE_INFO
//  default-initialises those four to 0 and another unit is responsible
//  for seeding them from the identify data; no default is substituted
//  here, so the comparisons keep their upstream meaning (a 0 threshold
//  disables the check).
// ═══════════════════════════════════════════════════════════════════════

DWORD CAtaSmart::CheckDiskStatus(const DRIVE_INFO& asi)
{
	// IsRawValues7 / IsRawValues8 — reconstructed from SmartKeyName, see
	// the banner.  Upstream sets IsRawValues8 for SmartJMicron60x and
	// SmartIndilinx, IsRawValues7 for SmartSandForce.
	const BOOL rawValues8 = (asi.SmartKeyName == L"SmartIndilinx" || asi.SmartKeyName == L"SmartJMicron60x");
	const BOOL rawValues7 = (asi.SmartKeyName == L"SmartSandForce");
	(void)rawValues7;   // no branch below consults it (upstream reads it in FillSmartData)

	// FlagLife* side channel — see CdiSmartDetail.h
	const cdi::detail::SsdLifeCtx lifeCtx = cdi::detail::LifeCtxOf(asi);

	// NVMe
	if (asi.DiskVendorId == SSD_VENDOR_NVME)
	{
		// https://github.com/hiyohiyo/CrystalDiskInfo/issues/99
		if (cdi::detail::StartsWith(asi.Model, L"Parallels")
		||  cdi::detail::StartsWith(asi.Model, L"VMware")
		||  cdi::detail::StartsWith(asi.Model, L"QEMU")
		)
		{
			return TraceAndReturn("nvme virtual", DISK_STATUS_UNKNOWN);
		}

		if (asi.Attribute[0].RawValue[0] > 0)
		{
			return TraceAndReturn("nvme critical warning", DISK_STATUS_BAD);
		}

		if (asi.Attribute[3].RawValue[0] == 0 || asi.Attribute[3].RawValue[0] > 100) // Available Spare Threshold does not available.
		{
		}
		else if (asi.Attribute[2].RawValue[0] < asi.Attribute[3].RawValue[0])
		{
			return TraceAndReturn("nvme spare below threshold", DISK_STATUS_BAD);
		}
		else if (asi.Attribute[2].RawValue[0] == asi.Attribute[3].RawValue[0] && asi.Attribute[3].RawValue[0] != 100)
		{
			return TraceAndReturn("nvme spare at threshold", DISK_STATUS_CAUTION);
		}

		if (asi.Life > asi.ThresholdFF)
		{
			return TraceAndReturn("nvme life above threshold", DISK_STATUS_GOOD);
		}
		else if (asi.Life <= asi.ThresholdFF)
		{
			return TraceAndReturn("nvme life at/below threshold", DISK_STATUS_CAUTION);
		}
	}

	if(! asi.IsSmartCorrect)
	{
		return TraceAndReturn("smart not correct", DISK_STATUS_UNKNOWN);
	}
	else if(! asi.IsSsd && ! asi.IsThresholdCorrect) // HDD
	{
		return TraceAndReturn("hdd threshold not correct", DISK_STATUS_UNKNOWN);
	}
	else if(asi.IsThresholdBug)
	{
		return TraceAndReturn("threshold bug", DISK_STATUS_UNKNOWN);
	}

	// DEBUG //
	// asi.Attribute[3].RawValue[0] = rand() % 256;

	int error = 0;
	int caution = 0;
	BOOL flagUnknown = TRUE;

	for (DWORD j = 0; j < asi.AttributeCount; j++)
	{
		// Check overlap
		for(DWORD k = 0; k < j; k++)
		{
			if(asi.Attribute[k].Id != 0 && asi.Attribute[j].Id == asi.Attribute[k].Id)
			{
				return TraceAndReturn("duplicate attribute id", DISK_STATUS_UNKNOWN);
			}
		}

		// Read Error Rate Bug
		if(asi.DiskVendorId == SSD_VENDOR_SANDFORCE && asi.Attribute[j].Id == 0x01
			&& asi.Attribute[j].CurrentValue == 0 && asi.Attribute[j].RawValue[0] == 0 && asi.Attribute[j].RawValue[1] == 0)
		{
		}
		// [2021/12/15] Workaround for SanDisk USB Memory
		else if (asi.Attribute[j].Id == 0xE8 && lifeCtx.FlagLifeSanDiskUsbMemory)
		{

		}
		// Temperature Threshold Bug
		else if(asi.Attribute[j].Id == 0xC2)
		{
		}
		else if(asi.IsSsd && rawValues8)
		{
		}
		else if(asi.IsSsd && ! rawValues8
		&&	asi.Threshold[j].ThresholdValue != 0
		&& 	asi.Attribute[j].CurrentValue < asi.Threshold[j].ThresholdValue)
		{
			error++;
		}
		else if((
			(0x01 <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0x0D)
		||	asi.Attribute[j].Id == 0x16
		||	(0xBB <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xBD)
		||	(0xBF <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xC1)
		||	(0xC3 <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xD1)
		||	(0xD3 <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xD4)
		||	(0xDC <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xE4)
		||	(0xE6 <= asi.Attribute[j].Id && asi.Attribute[j].Id <= 0xE7)
		||	asi.Attribute[j].Id == 0xF0
		||	asi.Attribute[j].Id == 0xFA
		||	asi.Attribute[j].Id == 0xFE
		)
		&&	asi.Threshold[j].ThresholdValue != 0
		&& 	asi.Attribute[j].CurrentValue < asi.Threshold[j].ThresholdValue)
		{
			error++;
		}

		if(asi.IsSsd && asi.Threshold[j].ThresholdValue != 0)
		{
			flagUnknown = FALSE;
		}

		if( asi.Attribute[j].Id == 0x05 // Reallocated Sectors Count
		||	asi.Attribute[j].Id == 0xC5 // Current Pending Sector Count
		||	asi.Attribute[j].Id == 0xC6 // Off-Line Scan Uncorrectable Sector Count
		)
		{
			if(asi.Attribute[j].RawValue[0] == 0xFF
			&& asi.Attribute[j].RawValue[1] == 0xFF
			&& asi.Attribute[j].RawValue[2] == 0xFF
			&& asi.Attribute[j].RawValue[3] == 0xFF)
			{
			}
			else
			{
				WORD raw = cdi::detail::B8toB16le(asi.Attribute[j].RawValue);
				WORD threshold = 0;
				switch(asi.Attribute[j].Id)
				{
				case 0x05:
					threshold = asi.Threshold05;
					break;
				case 0xC5:
					threshold = asi.ThresholdC5;
					break;
				case 0xC6:
					threshold = asi.ThresholdC6;
					break;
				}
				if(threshold > 0 && raw >= threshold && ! asi.IsSsd)
				{
					caution = 1;
				}
			}
			if(! asi.IsSsd)
			{
				flagUnknown = FALSE;
			}
		}
		else
		if(
		   (asi.Attribute[j].Id == 0xA9 && (asi.DiskVendorId == SSD_VENDOR_REALTEK || (asi.DiskVendorId == SSD_VENDOR_KINGSTON && asi.HostReadsWritesUnit == HOST_READS_WRITES_32MB /*KingstonKC600*/) || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION))
		|| (asi.Attribute[j].Id == 0xAD && asi.DiskVendorId == SSD_VENDOR_KIOXIA)
		|| (asi.Attribute[j].Id == 0xB1 && asi.DiskVendorId == SSD_VENDOR_SAMSUNG)
		|| (asi.Attribute[j].Id == 0xBB && asi.DiskVendorId == SSD_VENDOR_MTRON)
		|| (asi.Attribute[j].Id == 0xCA && (asi.DiskVendorId == SSD_VENDOR_MICRON || asi.DiskVendorId == SSD_VENDOR_MICRON_MU03 || asi.DiskVendorId == SSD_VENDOR_INTEL_DC || asi.DiskVendorId == SSD_VENDOR_SILICONMOTION_CVC))
		|| (asi.Attribute[j].Id == 0xD1 && asi.DiskVendorId == SSD_VENDOR_INDILINX)
		|| (asi.Attribute[j].Id == 0xE7 && (asi.DiskVendorId == SSD_VENDOR_SANDFORCE || asi.DiskVendorId == SSD_VENDOR_CORSAIR || asi.DiskVendorId == SSD_VENDOR_KINGSTON || asi.DiskVendorId == SSD_VENDOR_SKHYNIX
			                                || asi.DiskVendorId == SSD_VENDOR_REALTEK || asi.DiskVendorId == SSD_VENDOR_SANDISK || asi.DiskVendorId == SSD_VENDOR_SSSTC || asi.DiskVendorId == SSD_VENDOR_APACER || asi.DiskVendorId == SSD_VENDOR_PHISON
			                                || asi.DiskVendorId == SSD_VENDOR_JMICRON || asi.DiskVendorId == SSD_VENDOR_MAXIOTEK || asi.DiskVendorId == SSD_VENDOR_YMTC || asi.DiskVendorId == SSD_VENDOR_SCY || asi.DiskVendorId == SSD_VENDOR_RECADATA || asi.DiskVendorId == SSD_VENDOR_ADATA_INDUSTRIAL))
		|| (asi.Attribute[j].Id == 0xE8 && asi.DiskVendorId == SSD_VENDOR_PLEXTOR)
		|| (asi.Attribute[j].Id == 0xE9 && (asi.DiskVendorId == SSD_VENDOR_INTEL || asi.DiskVendorId == SSD_VENDOR_OCZ || asi.DiskVendorId == SSD_VENDOR_OCZ_VECTOR || asi.DiskVendorId == SSD_VENDOR_SKHYNIX))
		|| (asi.Attribute[j].Id == 0xE9 && asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO_HELEN_VENUS) || (asi.Attribute[j].Id == 0xE9
			&& asi.DiskVendorId == SSD_VENDOR_SAMSUNG
			&& cdi::detail::IsSamsungEnterpriseModel(asi.Model))
		)
		{
			flagUnknown = FALSE;
			int life = 0;

			if (lifeCtx.FlagLifeNoReport)
			{
				life = -1;
			}
			else if (lifeCtx.FlagLifeRawValueIncrement)
			{
				life = 100 - asi.Attribute[j].RawValue[0];
			}
			else if (lifeCtx.FlagLifeRawValue)
			{
				life = asi.Attribute[j].RawValue[0];
			}
			else
			{
				life = asi.Attribute[j].CurrentValue;
			}

			// if (life <= 0) { life = 0; }
			if (life > 100) { life = 100; }

			if (life == -1)
			{

			}
			else if(life == 0 || (! (lifeCtx.FlagLifeRawValue || lifeCtx.FlagLifeRawValueIncrement) && life < asi.Threshold[j].ThresholdValue))
			{
				error = 1;
			}
			else if(life <= asi.ThresholdFF)
			{
				caution = 1;
			}
		}
		else if(asi.Attribute[j].Id == 0xE6 && (asi.DiskVendorId == SSD_VENDOR_WDC || asi.DiskVendorId == SSD_VENDOR_SANDISK))
		{
			int life = 0;
			flagUnknown = FALSE;

			if (lifeCtx.FlagLifeSanDisk0_1)
			{
				life = 100 - (asi.Attribute[j].RawValue[1]*256 + asi.Attribute[j].RawValue[0])/100;
			}
			else if (lifeCtx.FlagLifeSanDisk1)
			{
				life = 100 - asi.Attribute[j].RawValue[1];
			}
			else
			{
				life = 100 - asi.Attribute[j].RawValue[1];
			}
			if (life <= 0) { life = 0; }
			if (life > 100) { life = 100; }

			if (lifeCtx.FlagLifeSanDiskUsbMemory)
			{

			}
			else if (life == 0)
			{
				error = 1;
			}
			else if(life <= asi.ThresholdFF)
			{
				caution = 1;
			}
		}
		else if (
			(asi.Attribute[j].Id == 0xE6 && (asi.DiskVendorId == SSD_VENDOR_SANDISK_LENOVO || asi.DiskVendorId == SSD_VENDOR_SANDISK_DELL))
		||  (asi.Attribute[j].Id == 0xC9 && (asi.DiskVendorId == SSD_VENDOR_SANDISK_HP || asi.DiskVendorId == SSD_VENDOR_SANDISK_HP_VENUS))
		)
		{
			int life = 0;
			flagUnknown = FALSE;

			life = asi.Attribute[j].CurrentValue;
			if (life <= 0) { life = 0; }
			if (life > 100) { life = 100; }
			// upstream also stores this into vars[i].Life (AtaSmart.cpp:12772);
			// the frozen `const DRIVE_INFO&` signature forbids the write here.
			// Nothing inside this function reads it back (the only reader is
			// the SSD_VENDOR_NVME pre-block above), so the result is unchanged.

			if (life == 0)
			{
				error = 1;
			}
			else if (life <= asi.ThresholdFF)
			{
				caution = 1;
			}
		}
	}


	/*
	if (asi.DiskVendorId == SSD_VENDOR_SAMSUNG)
	{
		// GetLifeByGpl(asi);
		if (asi.Life == -1)
		{

		}
		else if (asi.Life == 0)
		{
			error = 1;
		}
		else if (asi.Life <= asi.ThresholdFF)
		{
			caution = 1;
		}
	}
	*/

	if(error > 0)
	{
		return TraceAndReturn("error", DISK_STATUS_BAD);
	}
	else if(flagUnknown)
	{
		return TraceAndReturn("unknown", DISK_STATUS_UNKNOWN);
	}
	else if(caution > 0)
	{
		return TraceAndReturn("caution", DISK_STATUS_CAUTION);
	}
	else
	{
		return TraceAndReturn("good", DISK_STATUS_GOOD);
	}
}

// ═══════════════════════════════════════════════════════════════════════
//  CAtaSmart::GetPowerOnHours — CrystalDiskInfo AtaSmart.cpp:12862-12891
//
//  Semantics are frozen: src/CdiAttributeName.cpp:315 formats the return
//  value straight into the 0x09 attribute display, so the upstream
//  divisions are kept exactly as written.
// ═══════════════════════════════════════════════════════════════════════

DWORD CAtaSmart::GetPowerOnHours(DWORD rawValue, DWORD timeUnitType)
{
	switch(timeUnitType)
	{
	case POWER_ON_UNKNOWN:
		return 0;
		break;
	case POWER_ON_HOURS:
		return rawValue;
		break;
	case POWER_ON_MINUTES:
		return rawValue / 60;
		break;
	case POWER_ON_HALF_MINUTES:
		return rawValue / 120;
		break;
	case POWER_ON_SECONDS:
		return rawValue / 60 / 60;
		break;
	case POWER_ON_10_MINUTES:
		return rawValue / 6;
		break;
	case POWER_ON_MILLI_SECONDS:
		// upstream quirk: AtaSmart.cpp:12882 returns rawValue unchanged
		// here — no /1000 is applied even though the unit is milliseconds.
		// Kept as-is; "fixing" it would change the 0x09 display for every
		// Intel SSD 330/520, which upstream reports in raw milliseconds.
		return rawValue;
		break;
	default:
		return rawValue;
		break;
	}
}

// ═══════════════════════════════════════════════════════════════════════
//  CAtaSmart::MeasuredTimeUnit — CrystalDiskInfo AtaSmart.cpp:654-742
//
//  Samples PowerOnRawValue across a 125..155 second window and refines
//  MeasuredTimeUnitType.  Differences from upstream:
//    * GetTickCountFx()          -> ::GetTickCount64()
//    * vars array iteration      -> m_vars via GetDiskCount()/GetDisk()
//    * PowerOnStartRawValue      -> file-local map (see banner)
//    * MeasuredGetTickCount      -> file-local, seeded on first call
//    * IsMaxtorMinute            -> dropped (DRIVE_INFO has no such field)
//    * BOOL return               -> void (frozen header signature)
// ═══════════════════════════════════════════════════════════════════════

void CAtaSmart::MeasuredTimeUnit()
{
	const ULONGLONG getTickCount = ::GetTickCount64();

	// Capture the PowerOnStartRawValue baseline of every drive that this
	// function has not seen yet.  Upstream does this in AddDisk()
	// (AtaSmart.cpp:3716, `asi.PowerOnStartRawValue = asi.PowerOnRawValue`,
	// or -1 when !IsSmartCorrect), which this unit cannot hook; doing it
	// before the window check keeps the baseline in step with the window
	// that is opened just below.
	for (DWORD i = 0; i < GetDiskCount(); ++i)
	{
		const DRIVE_INFO& asi = GetDisk(i);
		if (s_powerOnStartRawValue.find(asi.PhysicalDriveId) == s_powerOnStartRawValue.end())
		{
			s_powerOnStartRawValue[asi.PhysicalDriveId] = asi.PowerOnRawValue;
		}
	}

	// Upstream: `if(getTickCount > MeasuredGetTickCount + 155000 ||
	// MeasuredGetTickCount + 125000 > getTickCount) return FALSE;` with
	// MeasuredGetTickCount set at the end of Init() (AtaSmart.cpp:2426).
	if (! s_measuredWindowOpened)
	{
		s_measuredWindowOpened = TRUE;
		s_measuredGetTickCount = getTickCount;
		SMART_TRACE_EVENT("MeasuredTimeUnit", SmartTraceCategory::DATA_REFRESH,
		                  "measurement window opened");
		return;
	}

	if(getTickCount > s_measuredGetTickCount + 155000 || s_measuredGetTickCount + 125000 > getTickCount)
	{
		SMART_TRACE_EVENT("MeasuredTimeUnit", SmartTraceCategory::DATA_REFRESH,
		                  "measurement window not open");
		return;
	}

	for(DWORD i = 0; i < GetDiskCount(); i++)
	{
		const DRIVE_INFO& asi = GetDisk(i);
		if(asi.PowerOnRawValue < 0)
		{
			continue;
		}
		UpdateSmartInfo(i);

		const std::map<INT, INT>::const_iterator baseline = s_powerOnStartRawValue.find(asi.PhysicalDriveId);
		if (baseline == s_powerOnStartRawValue.end())
		{
			continue;
		}
		DWORD test = static_cast<DWORD>(asi.PowerOnRawValue - baseline->second);

		if(asi.DetectedTimeUnitType == POWER_ON_MILLI_SECONDS)
		{
			m_vars[i].MeasuredTimeUnitType = POWER_ON_MILLI_SECONDS;
		}
		else if(asi.DetectedTimeUnitType == POWER_ON_10_MINUTES)
		{
			m_vars[i].MeasuredTimeUnitType = POWER_ON_10_MINUTES;
		}
		else if(asi.DiskVendorId == SSD_VENDOR_INDILINX)
		{
			m_vars[i].MeasuredTimeUnitType = POWER_ON_HOURS;
		}
		else if(cdi::detail::StartsWith(asi.Model, L"SAMSUNG"))
		{
			if(test >= 2)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HALF_MINUTES;
			}
			else
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HOURS;
			}
		}
		else if(cdi::detail::StartsWith(asi.Model, L"FUJITSU"))
		{
			if(test >= 6)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_SECONDS;
			}
			else if(test >= 4)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HALF_MINUTES;
			}
			else if(test >= 2)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_MINUTES;
			}
			else
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HOURS;
			}
		}
		else if(cdi::detail::StartsWith(asi.Model, L"MAXTOR"))
		{
			if(test >= 2)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_MINUTES;
				// upstream: vars[i].IsMaxtorMinute = TRUE;  (no such field)
			}
			else
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HOURS;
				// upstream: vars[i].IsMaxtorMinute = FALSE; (no such field)
			}
		}
		else
		{
			if(test >= 2)
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_MINUTES;
			}
			else
			{
				m_vars[i].MeasuredTimeUnitType = POWER_ON_HOURS;
			}
		}

		{
			char msg[128];
			sprintf_s(msg, "drive %d -> unit %u (delta %u)",
			          static_cast<int>(asi.PhysicalDriveId),
			          static_cast<unsigned>(m_vars[i].MeasuredTimeUnitType),
			          static_cast<unsigned>(test));
			SMART_TRACE_EVENT("MeasuredTimeUnit", SmartTraceCategory::DATA_REFRESH, msg);
		}
	}

	// upstream: return TRUE;
}

// ═══════════════════════════════════════════════════════════════════════
//  detail helpers
// ═══════════════════════════════════════════════════════════════════════

namespace detail {

// ── GetTimeUnitType — CrystalDiskInfo AtaSmart.cpp:12990-13072 ────────
//
// Upstream takes `CString model` by value and calls model.MakeUpper(), so
// every model.Find() below tests the upper-cased copy while `firmware` is
// left as it was.  That distinction is preserved: `upper` stands in for
// the mutated `model`, `firmware` is used raw.
DWORD GetTimeUnitType(const std::wstring& model, const std::wstring& firmware,
                      DWORD major, DWORD transferMode)
{
	const std::wstring upper = ToUpperCopy(model);

	if(StartsWith(upper, L"FUJITSU"))
	{
		if(major >= 8)
		{
			return POWER_ON_HOURS;
		}
		else
		{
			return POWER_ON_SECONDS;
		}
	}
	else if(StartsWith(upper, L"HITACHI_DK"))
	{
		return POWER_ON_MINUTES;
	}
	else if(StartsWith(upper, L"MAXTOR"))
	{
		if(transferMode >= cdi::detail::TRANSFER_MODE_SATA_300
		|| StartsWith(upper, L"MAXTOR 6H")		// Maxtor DiamondMax 11 family
		|| StartsWith(upper, L"MAXTOR 7H500")	// Maxtor MaXLine Pro 500 family
		|| StartsWith(upper, L"MAXTOR 6L0")		// Maxtor DiamondMax Plus D740X family
		|| StartsWith(upper, L"MAXTOR 4K")		// Maxtor DiamondMax D540X-4K family
		)
		{
			return POWER_ON_HOURS;
		}
		else
		{
			return POWER_ON_MINUTES;
		}
	}
	else if(StartsWith(upper, L"SAMSUNG"))
	{
		if(transferMode >= cdi::detail::TRANSFER_MODE_SATA_300)
		{
			return POWER_ON_HOURS;
		}
		else if(-23 >= _wtoi(cdi::detail::Right(firmware, 3).c_str()) && _wtoi(cdi::detail::Right(firmware, 3).c_str()) >= -39)
		{
			return POWER_ON_HALF_MINUTES;
		}
		else if(StartsWith(upper, L"SAMSUNG SV")
		||		StartsWith(upper, L"SAMSUNG SP")
		||		StartsWith(upper, L"SAMSUNG HM")
		||		StartsWith(upper, L"SAMSUNG MP")
		)
		{
			return POWER_ON_HALF_MINUTES;
		}
		else
		{
			return POWER_ON_HOURS;
		}
	}
	// 2012/1/15
	// https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=504;id=diskinfo#504
	// http://sourceforge.jp/ticket/browse.php?group_id=4394&tid=27443
	else if(
	   ((StartsWith(upper, L"CFD_CSSD-S6TM128NMPQ") || StartsWith(upper, L"CFD_CSSD-S6TM256NMPQ")) && (StartsWith(firmware, L"VM21") || StartsWith(firmware, L"VN21")))
	// upstream quirk: the model has been upper-cased but these two literals
	// have not, so `PX-128M2P` / `PX-256M2P` can never match — kept verbatim.
	|| ((Contains(upper, L"PX-128M2P") || Contains(upper, L"PX-256M2P")) && wcstod(firmware.c_str(), nullptr) < 1.059)
	// upstream quirk: same here — "Corsair Performance Pro" is tested
	// against the upper-cased model and never matches.  Kept verbatim.
	|| (StartsWith(upper, L"Corsair Performance Pro") && wcstod(firmware.c_str(), nullptr) < 1.059)
	)
	{
		return POWER_ON_10_MINUTES;
	}
	// https://crystalmark.info/bbs/c-board.cgi?cmd=one;no=1174;id=diskinfo#1174
	else if(
		   (StartsWith(upper, L"INTEL SSDSC2CW") && ContainsAfter0(upper, L"A3")) // Intel SSD 520 Series
		|| (StartsWith(upper, L"INTEL SSDSC2BW") && ContainsAfter0(upper, L"A3")) // Intel SSD 520 Series
		|| (StartsWith(upper, L"INTEL SSDSC2CT") && ContainsAfter0(upper, L"A3")) // Intel SSD 330 Series
		)
	{
		return POWER_ON_MILLI_SECONDS;
	}
	else
	{
		return POWER_ON_HOURS;
	}
}

// ── GetAtaMajorVersion — CrystalDiskInfo AtaSmart.cpp:13074-13118 ─────
DWORD GetAtaMajorVersion(WORD w80, std::wstring& majorVersion)
{
	DWORD major = 0;

	if(w80 == 0x0000 || w80 == 0xFFFF)
	{
		return 0;
	}

	for(int i = 14; i > 0; i--)
	{
		if((w80 >> i) & 0x1)
		{
			major = i;
			break;
		}
	}

	if (major >= 9)
	{
		majorVersion = FormatW(L"ACS-%d", static_cast<int>(major) - 7);
	}
	else if(major == 8)
	{
		majorVersion = L"ATA8-ACS";
	}
	else if(major >= 4)
	{
		majorVersion = FormatW(L"ATA/ATAPI-%d", static_cast<int>(major));
	}
	else if(major == 0)
	{
		majorVersion = L"----";
	}
	else
	{
		majorVersion = FormatW(L"ATA-%d", static_cast<int>(major));
	}

	return major;
}

// ── CheckAsciiStringError — CrystalDiskInfo AtaSmart.cpp:12843-12860 ──
//
// Ported verbatim, including the mutation: the first control character in
// 0x01..0x1F is replaced by a space and the scan stops there.  The scan
// also stops, returning TRUE, at the first byte >= 0x7f.
//
// upstream quirk: PCHAR is `char*`, which is signed on MSVC, so the
// `str[i] >= 0x7f` comparison only ever fires for the literal byte 0x7F.
// Bytes 0x80..0xFF read as negative and fall through the whole test.  The
// signed `char` type is therefore load-bearing and must not be widened to
// unsigned char / BYTE here.
BOOL CheckAsciiStringError(char* str, DWORD length)
{
	BOOL flag = FALSE;
	for(DWORD i = 0; i < length; i++)
	{
		if((0x00 < str[i] && str[i] < 0x20))
		{
			str[i] = 0x20;
			break;
		}
		else if(str[i] >= 0x7f)
		{
			flag = TRUE;
			break;
		}
	}
	return flag;
}

// ── WakeUpDisk — CrystalDiskInfo AtaSmart.cpp:6412-6433 ───────────────
//
// Upstream's CAtaSmart::WakeUp().  The FlagNoWakeUp global (+M 20211216)
// is dropped: DRIVE_INFO / CdiSmart.h have no equivalent, and the whole
// point of the function is to spin a parked disk up.
void WakeUpDisk(INT physicalDriveId)
{
	if(physicalDriveId < 0)
	{
		return ;
	}

	const std::wstring strDevice = FormatW(L"\\\\.\\PhysicalDrive%d", static_cast<int>(physicalDriveId));
	HANDLE hFile = ::CreateFileW(strDevice.c_str(), GENERIC_READ, FILE_SHARE_READ,
	                             NULL, OPEN_EXISTING, 0, NULL);
	if(hFile != INVALID_HANDLE_VALUE)
	{
		BYTE buf[512] = {};
		const DWORD bufSize = 512;
		DWORD readSize = 0;
		::SetFilePointer(hFile, 0, NULL, FILE_BEGIN);
		(void)::ReadFile(hFile, buf, bufSize, &readSize, NULL);
		::CloseHandle(hFile);
	}
}

// ── IsWindows10OrGreater ─────────────────────────────────────────────
//
// Upstream (Priscilla/UtilityFx.h:IsWindowsVersionOrGreaterFx) wraps
// VerifyVersionInfoW.  That API is manifest-shimmed: without a
// <supportedOS> entry for Windows 10 in the application manifest the
// kernel lies and reports Windows 8, so upstream's answer depends on how
// the host executable was linked rather than on the OS it runs on.  This
// port resolves RtlGetVersion from ntdll.dll at runtime instead, which is
// not shimmed, and compares the real major version.
BOOL IsWindows10OrGreater()
{
	// Declared locally rather than via <winternl.h>: only the leading
	// fields are needed, and the SDK header pulls in a large amount of
	// kernel-mode surface.  Layout matches RTL_OSVERSIONINFOW.
	typedef struct _CDI_RTL_OSVERSIONINFOW {
		ULONG  dwOSVersionInfoSize;
		ULONG  dwMajorVersion;
		ULONG  dwMinorVersion;
		ULONG  dwBuildNumber;
		ULONG  dwPlatformId;
		WCHAR  szCSDVersion[128];
	} CDI_RTL_OSVERSIONINFOW;

	typedef LONG (WINAPI *RtlGetVersionFn)(CDI_RTL_OSVERSIONINFOW*);

	const HMODULE hNtDll = ::GetModuleHandleW(L"ntdll.dll");
	if (hNtDll == NULL)
	{
		return FALSE;
	}

	const RtlGetVersionFn rtlGetVersion =
		reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(hNtDll, "RtlGetVersion"));
	if (rtlGetVersion == NULL)
	{
		// Symbol is present on every supported OS (Windows 2000+);
		// fall back to FALSE rather than guessing.
		return FALSE;
	}

	CDI_RTL_OSVERSIONINFOW osvi = {};
	osvi.dwOSVersionInfoSize = sizeof(osvi);
	if (rtlGetVersion(&osvi) != 0 /* STATUS_SUCCESS */)
	{
		return FALSE;
	}

	return osvi.dwMajorVersion >= 10 ? TRUE : FALSE;
}

} // namespace detail
} // namespace cdi
