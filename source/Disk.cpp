/*
AppleWin : An Apple //e emulator for Windows

Copyright (C) 1994-1996, Michael O'Brien
Copyright (C) 1999-2001, Oliver Schmidt
Copyright (C) 2002-2005, Tom Charlesworth
Copyright (C) 2006-2019, Tom Charlesworth, Michael Pohoreski, Nick Westgate

AppleWin is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

AppleWin is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with AppleWin; if not, write to the Free Software
Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

/* Description: Disk
 *
 * Author: Various
 *
 * In comments, UTAIIe is an abbreviation for a reference to "Understanding the Apple //e" by James Sather
 */

#include "StdAfx.h"

#include "SaveState_Structs_v1.h"

#include "Applewin.h"
#include "CPU.h"
#include "Disk.h"
#include "DiskImage.h"
#include "Frame.h"
#include "Log.h"
#include "Memory.h"
#include "Registry.h"
#include "Video.h"
#include "YamlHelper.h"

#include "../resource/resource.h"

// About m_enhanceDisk:
// . In general m_enhanceDisk==false is used for authentic disk access speed, whereas m_enhanceDisk==true is for enhanced speed.
// Details:
// . if false: Used by ImageReadTrack() to skew the sectors in a track (for .do, .dsk, .po 5.25" images).
// . if true && m_floppyMotorOn, then this is a condition for full-speed (unthrottled) emulation mode.
// . if false && I/O ReadWrite($C0EC) && drive is spinning, then advance the track buffer's nibble index (to simulate spinning).
// . if I/O ReadWrite($C0EC) && read, then depending on true/false support partial nibble reads for different gaps between consecutive accesses.
// Also m_enhanceDisk is persisted to the save-state, so it's an attribute of the DiskII interface card.

Disk2InterfaceCard::Disk2InterfaceCard(void)
{
	m_currDrive = 0;
	m_floppyLatch = 0;
	m_floppyMotorOn = 0;
	m_floppyLoadMode = 0;
	m_floppyWriteMode = 0;
	m_phases = 0;
	m_saveDiskImage = true;	// Save the DiskImage name to Registry
	m_slot = 0;
	m_diskLastCycle = 0;
	m_diskLastReadLatchCycle = 0;
	m_enhanceDisk = true;

	// Debug:
#if LOG_DISK_NIBBLES_USE_RUNTIME_VAR
	m_bLogDisk_NibblesRW = false;
#endif
#if LOG_DISK_NIBBLES_WRITE
	m_uWriteLastCycle = 0;
	m_uSyncFFCount = 0;
#endif
}

bool Disk2InterfaceCard::GetEnhanceDisk(void) { return m_enhanceDisk; }
void Disk2InterfaceCard::SetEnhanceDisk(bool bEnhanceDisk) { m_enhanceDisk = bEnhanceDisk; }

int Disk2InterfaceCard::GetCurrentDrive(void)  { return m_currDrive; }
int Disk2InterfaceCard::GetCurrentTrack(void)  { return m_floppyDrive[m_currDrive].m_track; }
int Disk2InterfaceCard::GetCurrentPhase(void)  { return m_floppyDrive[m_currDrive].m_phase; }
int Disk2InterfaceCard::GetCurrentOffset(void) { return m_floppyDrive[m_currDrive].m_disk.m_byte; }
int Disk2InterfaceCard::GetTrack(const int drive)  { return m_floppyDrive[drive].m_track; }

LPCTSTR Disk2InterfaceCard::GetCurrentState(void)
{
	if (m_floppyDrive[m_currDrive].m_disk.m_imagehandle == NULL)
		return "Empty";

	if (!m_floppyMotorOn)
	{
		if (m_floppyDrive[m_currDrive].m_spinning > 0)
			return "Off (spinning)";
		else
			return "Off";
	}
	else if (m_floppyWriteMode)
	{
		if (m_floppyDrive[m_currDrive].m_disk.m_bWriteProtected)
			return "Writing (write protected)";
		else
			return "Writing";
	}
	else
	{
		/*if (m_floppyLoadMode)
		{
			if (m_floppyDrive[m_currDrive].disk.bWriteProtected)
				return "Reading write protect state (write protected)";
			else
				return "Reading write protect state (not write protected)";
		}
		else*/
			return "Reading";
	}
}

//===========================================================================

void Disk2InterfaceCard::LoadLastDiskImage(const int drive)
{
	_ASSERT(drive == DRIVE_1 || drive == DRIVE_2);

	char sFilePath[ MAX_PATH + 1];
	sFilePath[0] = 0;

	const char *pRegKey = (drive == DRIVE_1)
		? REGVALUE_PREF_LAST_DISK_1
		: REGVALUE_PREF_LAST_DISK_2;

	if (RegLoadString(TEXT(REG_PREFS), pRegKey, 1, sFilePath, MAX_PATH))
	{
		sFilePath[ MAX_PATH ] = 0;

		m_saveDiskImage = false;
		// Pass in ptr to local copy of filepath, since RemoveDisk() sets DiskPathFilename = ""
		InsertDisk(drive, sFilePath, IMAGE_USE_FILES_WRITE_PROTECT_STATUS, IMAGE_DONT_CREATE);
		m_saveDiskImage = true;
	}
}

//===========================================================================

void Disk2InterfaceCard::SaveLastDiskImage(const int drive)
{
	_ASSERT(drive == DRIVE_1 || drive == DRIVE_2);

	if (!m_saveDiskImage)
		return;

	const char *pFileName = m_floppyDrive[drive].m_disk.m_fullname;

	if (drive == DRIVE_1)
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_DISK_1, TRUE, pFileName);
	else
		RegSaveString(TEXT(REG_PREFS), REGVALUE_PREF_LAST_DISK_2, TRUE, pFileName);

	//

	char szPathName[MAX_PATH];
	strcpy(szPathName, DiskGetFullPathName(drive));
	if (_tcsrchr(szPathName, TEXT('\\')))
	{
		char* pPathEnd = _tcsrchr(szPathName, TEXT('\\'))+1;
		*pPathEnd = 0;
		RegSaveString(TEXT(REG_PREFS), TEXT(REGVALUE_PREF_START_DIR), 1, szPathName);
	}
}

//===========================================================================

// Called by ControlMotor() & Enable()
void Disk2InterfaceCard::CheckSpinning(const ULONG nExecutedCycles)
{
	DWORD modechange = (m_floppyMotorOn && !m_floppyDrive[m_currDrive].m_spinning);

	if (m_floppyMotorOn)
		m_floppyDrive[m_currDrive].m_spinning = SPINNING_CYCLES;

	if (modechange)
		FrameDrawDiskLEDS( (HDC)0 );

	if (modechange)
	{
		// Set m_diskLastCycle when motor changes: not spinning (ie. off for 1 sec) -> on
		CpuCalcCycles(nExecutedCycles);
		m_diskLastCycle = g_nCumulativeCycles;
	}
}

//===========================================================================

Disk_Status_e Disk2InterfaceCard::GetDriveLightStatus(const int drive)
{
	if (IsDriveValid( drive ))
	{
		FloppyDrive* pDrive = &m_floppyDrive[ drive ];

		if (pDrive->m_spinning)
		{
			if (pDrive->m_disk.m_bWriteProtected)
				return DISK_STATUS_PROT;

			if (pDrive->m_writelight)
				return DISK_STATUS_WRITE;
			else
				return DISK_STATUS_READ;
		}
		else
		{
			return DISK_STATUS_OFF;
		}
	}

	return DISK_STATUS_OFF;
}

//===========================================================================

bool Disk2InterfaceCard::IsDriveValid(const int drive)
{
	return (drive >= 0 && drive < NUM_DRIVES);
}

//===========================================================================

void Disk2InterfaceCard::AllocTrack(const int drive)
{
	FloppyDisk* pFloppy = &m_floppyDrive[drive].m_disk;
	pFloppy->m_trackimage = (LPBYTE)VirtualAlloc(NULL, NIBBLES_PER_TRACK, MEM_COMMIT, PAGE_READWRITE);
}

//===========================================================================

void Disk2InterfaceCard::ReadTrack(const int drive)
{
	if (! IsDriveValid( drive ))
		return;

	FloppyDrive* pDrive = &m_floppyDrive[ drive ];
	FloppyDisk* pFloppy = &pDrive->m_disk;

	if (pDrive->m_track >= ImageGetNumTracks(pFloppy->m_imagehandle))
	{
		pFloppy->m_trackimagedata = false;
		return;
	}

	if (!pFloppy->m_trackimage)
		AllocTrack( drive );

	if (pFloppy->m_trackimage && pFloppy->m_imagehandle)
	{
#if LOG_DISK_TRACKS
		LOG_DISK("track $%02X%s read\r\n", pDrive->m_track, (pDrive->m_phase & 1) ? ".5" : "  ");
#endif
		ImageReadTrack(
			pFloppy->m_imagehandle,
			pDrive->m_track,
			pDrive->m_phase,
			pFloppy->m_trackimage,
			&pFloppy->m_nibbles,
			m_enhanceDisk);

		pFloppy->m_byte           = 0;
		pFloppy->m_trackimagedata = (pFloppy->m_nibbles != 0);
	}
}

//===========================================================================

void Disk2InterfaceCard::RemoveDisk(const int drive)
{
	FloppyDisk* pFloppy = &m_floppyDrive[drive].m_disk;

	if (pFloppy->m_imagehandle)
	{
		FlushCurrentTrack(drive);

		ImageClose(pFloppy->m_imagehandle);
		pFloppy->m_imagehandle = NULL;
	}

	if (pFloppy->m_trackimage)
	{
		VirtualFree(pFloppy->m_trackimage, 0, MEM_RELEASE);
		pFloppy->m_trackimage     = NULL;
		pFloppy->m_trackimagedata = false;
	}

	memset( pFloppy->m_imagename, 0, MAX_DISK_IMAGE_NAME+1 );
	memset( pFloppy->m_fullname , 0, MAX_DISK_FULL_NAME +1 );
	pFloppy->m_strFilenameInZip = "";

	SaveLastDiskImage( drive );
	Video_ResetScreenshotCounter( NULL );
}

//===========================================================================

void Disk2InterfaceCard::WriteTrack(const int drive)
{
	FloppyDrive* pDrive = &m_floppyDrive[ drive ];
	FloppyDisk* pFloppy = &pDrive->m_disk;

	if (pDrive->m_track >= ImageGetNumTracks(pFloppy->m_imagehandle))
		return;

	if (pFloppy->m_bWriteProtected)
		return;

	if (pFloppy->m_trackimage && pFloppy->m_imagehandle)
	{
#if LOG_DISK_TRACKS
		LOG_DISK("track $%02X%s write\r\n", pDrive->m_track, (pDrive->m_phase & 0) ? ".5" : "  "); // TODO: hard-coded to whole tracks - see below (nickw)
#endif
		ImageWriteTrack(
			pFloppy->m_imagehandle,
			pDrive->m_track,
			pDrive->m_phase, // TODO: this should never be used; it's the current phase (half-track), not that of the track to be written (nickw)
			pFloppy->m_trackimage,
			pFloppy->m_nibbles);
	}

	pFloppy->m_trackimagedirty = false;
}

void Disk2InterfaceCard::FlushCurrentTrack(const int drive)
{
	FloppyDisk* pFloppy = &m_floppyDrive[drive].m_disk;

	if (pFloppy->m_trackimage && pFloppy->m_trackimagedirty)
		WriteTrack(drive);
}

//===========================================================================

void Disk2InterfaceCard::Boot(void)
{
	// THIS FUNCTION RELOADS A PROGRAM IMAGE IF ONE IS LOADED IN DRIVE ONE.
	// IF A DISK IMAGE OR NO IMAGE IS LOADED IN DRIVE ONE, IT DOES NOTHING.
	if (m_floppyDrive[0].m_disk.m_imagehandle && ImageBoot(m_floppyDrive[0].m_disk.m_imagehandle))
		m_floppyMotorOn = 0;
}

//===========================================================================

void __stdcall Disk2InterfaceCard::ControlMotor(WORD, WORD address, BYTE, BYTE, ULONG uExecutedCycles)
{
	BOOL newState = address & 1;

	if (newState != m_floppyMotorOn)	// motor changed state
		m_formatTrack.DriveNotWritingTrack();

	m_floppyMotorOn = newState;
	// NB. Motor off doesn't reset the Command Decoder like reset. (UTAIIe figures 9.7 & 9.8 chip C2)
	// - so it doesn't reset this state: m_floppyLoadMode, m_floppyWriteMode, m_phases
#if LOG_DISK_MOTOR
	LOG_DISK("motor %s\r\n", (m_floppyMotorOn) ? "on" : "off");
#endif
	CheckSpinning(uExecutedCycles);
}

//===========================================================================

void __stdcall Disk2InterfaceCard::ControlStepper(WORD, WORD address, BYTE, BYTE, ULONG uExecutedCycles)
{
	FloppyDrive* pDrive = &m_floppyDrive[m_currDrive];
	FloppyDisk* pFloppy = &pDrive->m_disk;

	if (!m_floppyMotorOn)	// GH#525
	{
		if (!pDrive->m_spinning)
		{
#if LOG_DISK_PHASES
			LOG_DISK("stepper accessed whilst motor is off and not spinning\r\n");
#endif
			return;
		}

#if LOG_DISK_PHASES
		LOG_DISK("stepper accessed whilst motor is off, but still spinning\r\n");
#endif
	}

	int phase = (address >> 1) & 3;
	int phase_bit = (1 << phase);

#if 1
	// update the magnet states
	if (address & 1)
	{
		// phase on
		m_phases |= phase_bit;
	}
	else
	{
		// phase off
		m_phases &= ~phase_bit;
	}

	// check for any stepping effect from a magnet
	// - move only when the magnet opposite the cog is off
	// - move in the direction of an adjacent magnet if one is on
	// - do not move if both adjacent magnets are on
	// momentum and timing are not accounted for ... maybe one day!
	int direction = 0;
	if (m_phases & (1 << ((pDrive->m_phase + 1) & 3)))
		direction += 1;
	if (m_phases & (1 << ((pDrive->m_phase + 3) & 3)))
		direction -= 1;

	// apply magnet step, if any
	if (direction)
	{
		pDrive->m_phase = MAX(0, MIN(79, pDrive->m_phase + direction));
		const int nNumTracksInImage = ImageGetNumTracks(pFloppy->m_imagehandle);
		const int newtrack = (nNumTracksInImage == 0)	? 0
														: MIN(nNumTracksInImage-1, pDrive->m_phase >> 1); // (round half tracks down)
		if (newtrack != pDrive->m_track)
		{
			FlushCurrentTrack(m_currDrive);
			pDrive->m_track = newtrack;
			pFloppy->m_trackimagedata = false;

			m_formatTrack.DriveNotWritingTrack();
		}

		// Feature Request #201 Show track status
		// https://github.com/AppleWin/AppleWin/issues/201
		FrameDrawDiskStatus( (HDC)0 );
	}
#else
	// substitute alternate stepping code here to test
#endif

#if LOG_DISK_PHASES
	LOG_DISK("track $%02X%s phases %d%d%d%d phase %d %s address $%4X\r\n",
		pDrive->m_phase >> 1,
		(pDrive->m_phase & 1) ? ".5" : "  ",
		(m_phases >> 3) & 1,
		(m_phases >> 2) & 1,
		(m_phases >> 1) & 1,
		(m_phases >> 0) & 1,
		phase,
		(address & 1) ? "on " : "off",
		address);
#endif
}

//===========================================================================

void Disk2InterfaceCard::Destroy(void)
{
	m_saveDiskImage = false;
	RemoveDisk(DRIVE_1);

	m_saveDiskImage = false;
	RemoveDisk(DRIVE_2);

	m_saveDiskImage = true;
}

//===========================================================================

void __stdcall Disk2InterfaceCard::Enable(WORD, WORD address, BYTE, BYTE, ULONG uExecutedCycles)
{
	m_currDrive = address & 1;
#if LOG_DISK_ENABLE_DRIVE
	LOG_DISK("enable drive: %d\r\n", m_currDrive);
#endif
	m_floppyDrive[!m_currDrive].m_spinning   = 0;
	m_floppyDrive[!m_currDrive].m_writelight = 0;
	CheckSpinning(uExecutedCycles);
}

//===========================================================================

void Disk2InterfaceCard::EjectDisk(const int drive)
{
	if (IsDriveValid(drive))
	{
		RemoveDisk(drive);
	}
}

//===========================================================================

// Return the filename
// . Used by Drive Buttons' tooltips
LPCTSTR Disk2InterfaceCard::GetFullDiskFilename(const int drive)
{
	if (!m_floppyDrive[drive].m_disk.m_strFilenameInZip.empty())
		return m_floppyDrive[drive].m_disk.m_strFilenameInZip.c_str();

	return GetFullName(drive);
}

// Return the file or zip name
// . Used by Property Sheet Page (Disk)
LPCTSTR Disk2InterfaceCard::GetFullName(const int drive)
{
	return m_floppyDrive[drive].m_disk.m_fullname;
}

// Return the imagename
// . Used by Drive Button's icons & Property Sheet Page (Save snapshot)
LPCTSTR Disk2InterfaceCard::GetBaseName(const int drive)
{
	return m_floppyDrive[drive].m_disk.m_imagename;
}

LPCTSTR Disk2InterfaceCard::DiskGetFullPathName(const int drive)
{
	return ImageGetPathname(m_floppyDrive[drive].m_disk.m_imagehandle);
}

//===========================================================================

void Disk2InterfaceCard::GetLightStatus(Disk_Status_e *pDisk1Status, Disk_Status_e *pDisk2Status)
{
	if (pDisk1Status)
		*pDisk1Status = GetDriveLightStatus(DRIVE_1);

	if (pDisk2Status)
		*pDisk2Status = GetDriveLightStatus(DRIVE_2);
}

//===========================================================================

ImageError_e Disk2InterfaceCard::InsertDisk(const int drive, LPCTSTR pszImageFilename, const bool bForceWriteProtected, const bool bCreateIfNecessary)
{
	FloppyDrive* pDrive = &m_floppyDrive[drive];
	FloppyDisk* pFloppy = &pDrive->m_disk;

	if (pFloppy->m_imagehandle)
		RemoveDisk(drive);

	// Reset the disk's attributes, but preserve the drive's attributes (GH#138/Platoon, GH#640)
	// . Changing the disk (in the drive) doesn't affect the drive's attributes.
	pFloppy->clear();

	const DWORD dwAttributes = GetFileAttributes(pszImageFilename);
	if(dwAttributes == INVALID_FILE_ATTRIBUTES)
		pFloppy->m_bWriteProtected = false;	// Assume this is a new file to create
	else
		pFloppy->m_bWriteProtected = bForceWriteProtected ? true : (dwAttributes & FILE_ATTRIBUTE_READONLY);

	// Check if image is being used by the other drive, and if so remove it in order so it can be swapped
	{
		const char* pszOtherPathname = DiskGetFullPathName(!drive);

		char szCurrentPathname[MAX_PATH]; 
		DWORD uNameLen = GetFullPathName(pszImageFilename, MAX_PATH, szCurrentPathname, NULL);
		if (uNameLen == 0 || uNameLen >= MAX_PATH)
			strcpy_s(szCurrentPathname, MAX_PATH, pszImageFilename);

 		if (!strcmp(pszOtherPathname, szCurrentPathname))
		{
			EjectDisk(!drive);
			FrameRefreshStatus(DRAW_LEDS | DRAW_BUTTON_DRIVES);
		}
	}

	ImageError_e Error = ImageOpen(pszImageFilename,
		&pFloppy->m_imagehandle,
		&pFloppy->m_bWriteProtected,
		bCreateIfNecessary,
		pFloppy->m_strFilenameInZip);

	if (Error == eIMAGE_ERROR_NONE && ImageIsMultiFileZip(pFloppy->m_imagehandle))
	{
		TCHAR szText[100+MAX_PATH];
		szText[sizeof(szText)-1] = 0;
		_snprintf(szText, sizeof(szText)-1, "Only the first file in a multi-file zip is supported\nUse disk image '%s' ?", pFloppy->m_strFilenameInZip.c_str());
		int nRes = MessageBox(g_hFrameWindow, szText, TEXT("Multi-Zip Warning"), MB_ICONWARNING | MB_YESNO | MB_SETFOREGROUND);
		if (nRes == IDNO)
		{
			RemoveDisk(drive);
			Error = eIMAGE_ERROR_REJECTED_MULTI_ZIP;
		}
	}

	if (Error == eIMAGE_ERROR_NONE)
	{
		GetImageTitle(pszImageFilename, pFloppy->m_imagename, pFloppy->m_fullname);
		Video_ResetScreenshotCounter(pFloppy->m_imagename);
	}
	else
	{
		Video_ResetScreenshotCounter(NULL);
	}

	SaveLastDiskImage(drive);
	
	return Error;
}

//===========================================================================

bool Disk2InterfaceCard::IsConditionForFullSpeed(void)
{
	return m_floppyMotorOn && m_enhanceDisk;
}

//===========================================================================

void Disk2InterfaceCard::NotifyInvalidImage(const int drive, LPCTSTR pszImageFilename, const ImageError_e Error)
{
	TCHAR szBuffer[MAX_PATH+128];
	szBuffer[sizeof(szBuffer)-1] = 0;

	switch (Error)
	{
	case eIMAGE_ERROR_UNABLE_TO_OPEN:
	case eIMAGE_ERROR_UNABLE_TO_OPEN_GZ:
	case eIMAGE_ERROR_UNABLE_TO_OPEN_ZIP:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to open the file %s."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_BAD_SIZE:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the file %s\nbecause the ")
			TEXT("disk image is an unsupported size."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_BAD_FILE:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the file %s\nbecause the ")
			TEXT("OS can't access it."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_UNSUPPORTED:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the file %s\nbecause the ")
			TEXT("disk image format is not recognized."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_UNSUPPORTED_HDV:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the file %s\n")
			TEXT("because this UniDisk 3.5/Apple IIGS/hard-disk image is not supported.\n")
			TEXT("Try inserting as a hard-disk image instead."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_UNSUPPORTED_MULTI_ZIP:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the file %s\nbecause the ")
			TEXT("first file (%s) in this multi-zip archive is not recognized.\n")
			TEXT("Try unzipping and using the disk images directly.\n"),
			pszImageFilename,
			m_floppyDrive[drive].m_disk.m_strFilenameInZip.c_str());
		break;

	case eIMAGE_ERROR_GZ:
	case eIMAGE_ERROR_ZIP:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to use the compressed file %s\nbecause the ")
			TEXT("compressed disk image is corrupt/unsupported."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_FAILED_TO_GET_PATHNAME:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unable to GetFullPathName() for the file: %s."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_ZEROLENGTH_WRITEPROTECTED:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Unsupported zero-length write-protected file: %s."),
			pszImageFilename);
		break;

	case eIMAGE_ERROR_FAILED_TO_INIT_ZEROLENGTH:
		_snprintf(
			szBuffer,
			sizeof(szBuffer)-1,
			TEXT("Failed to resize the zero-length file: %s."),
			pszImageFilename);
		break;

	default:
		// IGNORE OTHER ERRORS SILENTLY
		return;
	}

	MessageBox(
		g_hFrameWindow,
		szBuffer,
		g_pAppTitle,
		MB_ICONEXCLAMATION | MB_SETFOREGROUND);
}

//===========================================================================

bool Disk2InterfaceCard::GetProtect(const int drive)
{
	if (IsDriveValid(drive))
	{
		if (m_floppyDrive[drive].m_disk.m_bWriteProtected)
			return true;
	}

	return false;
}

//===========================================================================

void Disk2InterfaceCard::SetProtect(const int drive, const bool bWriteProtect)
{
	if (IsDriveValid( drive ))
	{
		m_floppyDrive[drive].m_disk.m_bWriteProtected = bWriteProtect;
	}
}

//===========================================================================

bool Disk2InterfaceCard::IsDiskImageWriteProtected(const int drive)
{
	if (!IsDriveValid(drive))
		return true;

	return ImageIsWriteProtected(m_floppyDrive[drive].m_disk.m_imagehandle);
}

//===========================================================================

bool Disk2InterfaceCard::IsDriveEmpty(const int drive)
{
	if (!IsDriveValid(drive))
		return true;

	return m_floppyDrive[drive].m_disk.m_imagehandle == NULL;
}

//===========================================================================

#if LOG_DISK_NIBBLES_WRITE
bool Disk2InterfaceCard::LogWriteCheckSyncFF(ULONG& uCycleDelta)
{
	bool bIsSyncFF = false;

	if (m_uWriteLastCycle == 0)	// Reset to 0 when write mode is enabled
	{
		uCycleDelta = 0;
		if (m_floppyLatch == 0xFF)
		{
			m_uSyncFFCount = 0;
			bIsSyncFF = true;
		}
	}
	else
	{
		uCycleDelta = (ULONG) (g_nCumulativeCycles - m_uWriteLastCycle);
		if (m_floppyLatch == 0xFF && uCycleDelta > 32)
		{
			m_uSyncFFCount++;
			bIsSyncFF = true;
		}
	}

	m_uWriteLastCycle = g_nCumulativeCycles;
	return bIsSyncFF;
}
#endif

//===========================================================================

void __stdcall Disk2InterfaceCard::ReadWrite(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	/* m_floppyLoadMode = 0; */
	FloppyDrive* pDrive = &m_floppyDrive[m_currDrive];
	FloppyDisk* pFloppy = &pDrive->m_disk;

	if (!pFloppy->m_trackimagedata && pFloppy->m_imagehandle)
		ReadTrack(m_currDrive);

	if (!pFloppy->m_trackimagedata)
	{
		m_floppyLatch = 0xFF;
		return;
	}

	// Improve precision of "authentic" drive mode - GH#125
	UINT uSpinNibbleCount = 0;
	CpuCalcCycles(nExecutedCycles);	// g_nCumulativeCycles required for uSpinNibbleCount & LogWriteCheckSyncFF()

	if (!m_enhanceDisk && pDrive->m_spinning)
	{
		const ULONG nCycleDiff = (ULONG) (g_nCumulativeCycles - m_diskLastCycle);
		m_diskLastCycle = g_nCumulativeCycles;

		if (nCycleDiff > 40)
		{
			// 40 cycles for a write of a 10-bit 0xFF sync byte
			uSpinNibbleCount = nCycleDiff >> 5;	// ...but divide by 32 (not 40)

			ULONG uWrapOffset = uSpinNibbleCount % pFloppy->m_nibbles;
			pFloppy->m_byte += uWrapOffset;
			if (pFloppy->m_byte >= pFloppy->m_nibbles)
				pFloppy->m_byte -= pFloppy->m_nibbles;

#if LOG_DISK_NIBBLES_SPIN
			UINT uCompleteRevolutions = uSpinNibbleCount / pFloppy->m_nibbles;
			LOG_DISK("spin: revs=%d, nibbles=%d\r\n", uCompleteRevolutions, uWrapOffset);
#endif
		}
	}

	if (!m_floppyWriteMode)
	{
		// Don't change latch if drive off after 1 second drive-off delay (UTAIIe page 9-13)
		// "DRIVES OFF forces the data register to hold its present state." (UTAIIe page 9-12)
		// Note: Sherwood Forest sets shift mode and reads with the drive off.
		if (!pDrive->m_spinning)	// GH#599
			return;

		const ULONG nReadCycleDiff = (ULONG) (g_nCumulativeCycles - m_diskLastReadLatchCycle);

		// Support partial nibble read if disk reads are very close: (GH#582)
		// . 6 cycles (1st->2nd read) for DOS 3.3 / $BD34: "read with delays to see if disk is spinning." (Beneath Apple DOS)
		// . 6 cycles (1st->2nd read) for Curse of the Azure Bonds (loop to see if disk is spinning)
		// . 31 cycles is the max for a partial 8-bit nibble
		const ULONG kReadAccessThreshold = m_enhanceDisk ? 6 : 31;

		if (nReadCycleDiff <= kReadAccessThreshold)
		{
			UINT invalidBits = 8 - (nReadCycleDiff / 4);	// 4 cycles per bit-cell
			m_floppyLatch = *(pFloppy->m_trackimage + pFloppy->m_byte) >> invalidBits;
			return;	// Early return so don't update: m_diskLastReadLatchCycle & pFloppy->byte
		}

		m_floppyLatch = *(pFloppy->m_trackimage + pFloppy->m_byte);
		m_diskLastReadLatchCycle = g_nCumulativeCycles;

#if LOG_DISK_NIBBLES_READ
  #if LOG_DISK_NIBBLES_USE_RUNTIME_VAR
		if (m_bLogDisk_NibblesRW)
  #endif
		{
			LOG_DISK("read %04X = %02X\r\n", pFloppy->m_byte, m_floppyLatch);
		}

		m_formatTrack.DecodeLatchNibbleRead(m_floppyLatch);
#endif
	}
	else if (!pFloppy->m_bWriteProtected) // && m_floppyWriteMode
	{
		*(pFloppy->m_trackimage + pFloppy->m_byte) = m_floppyLatch;
		pFloppy->m_trackimagedirty = true;

		bool bIsSyncFF = false;
#if LOG_DISK_NIBBLES_WRITE
		ULONG uCycleDelta = 0;
		bIsSyncFF = LogWriteCheckSyncFF(uCycleDelta);
#endif

		m_formatTrack.DecodeLatchNibbleWrite(m_floppyLatch, uSpinNibbleCount, pFloppy, bIsSyncFF);	// GH#125

#if LOG_DISK_NIBBLES_WRITE
  #if LOG_DISK_NIBBLES_USE_RUNTIME_VAR
		if (m_bLogDisk_NibblesRW)
  #endif
		{
			if (!bIsSyncFF)
				LOG_DISK("write %04X = %02X (cy=+%d)\r\n", pFloppy->m_byte, m_floppyLatch, uCycleDelta);
			else
				LOG_DISK("write %04X = %02X (cy=+%d) sync #%d\r\n", pFloppy->m_byte, m_floppyLatch, uCycleDelta, m_uSyncFFCount);
		}
#endif
	}

	if (++pFloppy->m_byte >= pFloppy->m_nibbles)
		pFloppy->m_byte = 0;

	// Show track status (GH#201) - NB. Prevent flooding of forcing UI to redraw!!!
	if ((pFloppy->m_byte & 0xFF) == 0)
		FrameDrawDiskStatus( (HDC)0 );
}

//===========================================================================

void Disk2InterfaceCard::Reset(const bool bIsPowerCycle/*=false*/)
{
	// RESET forces all switches off (UTAIIe Table 9.1)
	m_currDrive = 0;
	m_floppyMotorOn = 0;
	m_floppyLoadMode = 0;
	m_floppyWriteMode = 0;
	m_phases = 0;

	m_formatTrack.Reset();

	if (bIsPowerCycle)	// GH#460
	{
		// NB. This doesn't affect the drive head (ie. drive's track position)
		// . The initial machine start-up state is track=0, but after a power-cycle the track could be any value.
		// . (For DiskII firmware, this results in a subtle extra latch read in this latter case, for the track!=0 case)

		m_floppyDrive[DRIVE_1].m_spinning   = 0;
		m_floppyDrive[DRIVE_1].m_writelight = 0;
		m_floppyDrive[DRIVE_2].m_spinning   = 0;
		m_floppyDrive[DRIVE_2].m_writelight = 0;

		FrameRefreshStatus(DRAW_LEDS, false);
	}
}

//===========================================================================

bool Disk2InterfaceCard::UserSelectNewDiskImage(const int drive, LPCSTR pszFilename/*=""*/)
{
	TCHAR directory[MAX_PATH] = TEXT("");
	TCHAR filename[MAX_PATH]  = TEXT("");
	TCHAR title[40];

	strcpy(filename, pszFilename);

	RegLoadString(TEXT(REG_PREFS), REGVALUE_PREF_START_DIR, 1, directory, MAX_PATH);
	_tcscpy(title, TEXT("Select Disk Image For Drive "));
	_tcscat(title, drive ? TEXT("2") : TEXT("1"));

	_ASSERT(sizeof(OPENFILENAME) == sizeof(OPENFILENAME_NT4));	// Required for Win98/ME support (selected by _WIN32_WINNT=0x0400 in stdafx.h)

	OPENFILENAME ofn;
	ZeroMemory(&ofn,sizeof(OPENFILENAME));
	ofn.lStructSize     = sizeof(OPENFILENAME);
	ofn.hwndOwner       = g_hFrameWindow;
	ofn.hInstance       = g_hInstance;
	ofn.lpstrFilter     = TEXT("All Images\0*.bin;*.do;*.dsk;*.nib;*.po;*.gz;*.zip;*.2mg;*.2img;*.iie;*.apl\0")
						  TEXT("Disk Images (*.bin,*.do,*.dsk,*.nib,*.po,*.gz,*.zip,*.2mg,*.2img,*.iie)\0*.bin;*.do;*.dsk;*.nib;*.po;*.gz;*.zip;*.2mg;*.2img;*.iie\0")
						  TEXT("All Files\0*.*\0");
	ofn.lpstrFile       = filename;
	ofn.nMaxFile        = MAX_PATH;
	ofn.lpstrInitialDir = directory;
	ofn.Flags           = OFN_PATHMUSTEXIST;
	ofn.lpstrTitle      = title;

	bool bRes = false;

	if (GetOpenFileName(&ofn))
	{
		if ((!ofn.nFileExtension) || !filename[ofn.nFileExtension])
			_tcscat(filename,TEXT(".dsk"));

		ImageError_e Error = InsertDisk(drive, filename, ofn.Flags & OFN_READONLY, IMAGE_CREATE);
		if (Error == eIMAGE_ERROR_NONE)
		{
			bRes = true;
		}
		else
		{
			NotifyInvalidImage(drive, filename, Error);
		}
	}

	return bRes;
}

//===========================================================================

void __stdcall Disk2InterfaceCard::LoadWriteProtect(WORD, WORD, BYTE write, BYTE value, ULONG)
{
	/* m_floppyLoadMode = 1; */

	// Don't change latch if drive off after 1 second drive-off delay (UTAIIe page 9-13)
	// "DRIVES OFF forces the data register to hold its present state." (UTAIIe page 9-12)
	// Note: Gemstone Warrior sets load mode with the drive off.
	if (!m_floppyDrive[m_currDrive].m_spinning)	// GH#599
		return;

	if (!write)
	{
		// Notes:
		// . Phase 1 on also forces write protect in the Disk II drive (UTAIIe page 9-7) but we don't implement that
		// . write mode doesn't prevent reading write protect (GH#537):
		//   "If for some reason the above write protect check were entered with the READ/WRITE switch in WRITE, 
		//    the write protect switch would still be read correctly" (UTAIIe page 9-21)
		if (m_floppyDrive[m_currDrive].m_disk.m_bWriteProtected)
			m_floppyLatch |= 0x80;
		else
			m_floppyLatch &= 0x7F;
	}
}

//===========================================================================

void __stdcall Disk2InterfaceCard::SetReadMode(WORD, WORD, BYTE, BYTE, ULONG)
{
	m_floppyWriteMode = 0;

	m_formatTrack.DriveSwitchedToReadMode(&m_floppyDrive[m_currDrive].m_disk);

#if LOG_DISK_RW_MODE
	LOG_DISK("rw mode: read\r\n");
#endif
}

//===========================================================================

void __stdcall Disk2InterfaceCard::SetWriteMode(WORD, WORD, BYTE, BYTE, ULONG uExecutedCycles)
{
	m_floppyWriteMode = 1;

	m_formatTrack.DriveSwitchedToWriteMode(m_floppyDrive[m_currDrive].m_disk.m_byte);

	BOOL modechange = !m_floppyDrive[m_currDrive].m_writelight;
#if LOG_DISK_RW_MODE
	LOG_DISK("rw mode: write (mode changed=%d)\r\n", modechange ? 1 : 0);
#endif
#if LOG_DISK_NIBBLES_WRITE
	m_uWriteLastCycle = 0;
#endif

	m_floppyDrive[m_currDrive].m_writelight = WRITELIGHT_CYCLES;

	if (modechange)
		FrameDrawDiskLEDS( (HDC)0 );
}

//===========================================================================

void Disk2InterfaceCard::UpdateDriveState(DWORD cycles)
{
	int loop = NUM_DRIVES;
	while (loop--)
	{
		FloppyDrive* pDrive = &m_floppyDrive[loop];

		if (pDrive->m_spinning && !m_floppyMotorOn)
		{
			if (!(pDrive->m_spinning -= MIN(pDrive->m_spinning, cycles)))
			{
				FrameDrawDiskLEDS( (HDC)0 );
				FrameDrawDiskStatus( (HDC)0 );
			}
		}

		if (m_floppyWriteMode && (m_currDrive == loop) && pDrive->m_spinning)
		{
			pDrive->m_writelight = WRITELIGHT_CYCLES;
		}
		else if (pDrive->m_writelight)
		{
			if (!(pDrive->m_writelight -= MIN(pDrive->m_writelight, cycles)))
			{
				FrameDrawDiskLEDS( (HDC)0 );
				FrameDrawDiskStatus( (HDC)0 );
			}
		}
	}
}

//===========================================================================

bool Disk2InterfaceCard::DriveSwap(void)
{
	// Refuse to swap if either Disk][ is active
	// TODO: if Shift-Click then FORCE drive swap to bypass message
	if (m_floppyDrive[DRIVE_1].m_spinning || m_floppyDrive[DRIVE_2].m_spinning)
	{
		// 1.26.2.4 Prompt when trying to swap disks while drive is on instead of silently failing
		int status = MessageBox(
			g_hFrameWindow,
			"WARNING:\n"
				"\n"
				"\tAttempting to swap a disk while a drive is on\n"
				"\t\t--> is NOT recommended <--\n"
				"\tas this will most likely read/write incorrect data!\n"
				"\n"
				"If the other drive is empty then swapping is harmless. The"
				" computer will appear to 'hang' trying to read non-existent data but"
				" you can safely swap disks once more to restore the original disk.\n"
				"\n"
				"Do you still wish to swap disks?",
			"Trying to swap a disk while a drive is on ...",
			MB_ICONWARNING | MB_YESNOCANCEL
		);

		switch( status )
		{
			case IDNO:
			case IDCANCEL:
				return false;
			default:
				break; // User is OK with swapping disks so let them proceed at their own risk
		}
	}

	FlushCurrentTrack(DRIVE_1);
	FlushCurrentTrack(DRIVE_2);

	// Swap disks between drives
	// . NB. We swap trackimage ptrs (so don't need to swap the buffers' data)
	std::swap(m_floppyDrive[DRIVE_1].m_disk, m_floppyDrive[DRIVE_2].m_disk);

	// Invalidate the trackimage so that a read latch will re-read the track for the new floppy (GH#543)
	m_floppyDrive[DRIVE_1].m_disk.m_trackimagedata = false;
	m_floppyDrive[DRIVE_2].m_disk.m_trackimagedata = false;

	SaveLastDiskImage(DRIVE_1);
	SaveLastDiskImage(DRIVE_2);

	FrameRefreshStatus(DRAW_LEDS | DRAW_BUTTON_DRIVES, false);

	return true;
}

//===========================================================================

// TODO: LoadRom_Disk_Floppy()
void Disk2InterfaceCard::Initialize(LPBYTE pCxRomPeripheral, UINT uSlot)
{
	const UINT DISK2_FW_SIZE = APPLE_SLOT_SIZE;

	HRSRC hResInfo = FindResource(NULL, MAKEINTRESOURCE(IDR_DISK2_FW), "FIRMWARE");
	if(hResInfo == NULL)
		return;

	DWORD dwResSize = SizeofResource(NULL, hResInfo);
	if(dwResSize != DISK2_FW_SIZE)
		return;

	HGLOBAL hResData = LoadResource(NULL, hResInfo);
	if(hResData == NULL)
		return;

	BYTE* pData = (BYTE*) LockResource(hResData);	// NB. Don't need to unlock resource
	if(pData == NULL)
		return;

	memcpy(pCxRomPeripheral + uSlot*APPLE_SLOT_SIZE, pData, DISK2_FW_SIZE);

	// Note: We used to disable the track stepping delay in the Disk II controller firmware by
	// patching $C64C with $A9,$00,$EA. Now not doing this since:
	// . Authentic Speed should be authentic
	// . Enhanced Speed runs emulation unthrottled, so removing the delay has negligible effect
	// . Patching the firmware breaks the ADC checksum used by "The CIA Files" (Tricky Dick)
	// . In this case we can patch to compensate for an ADC or EOR checksum but not both (nickw)

	RegisterIoHandler(uSlot, &Disk2InterfaceCard::IORead, &Disk2InterfaceCard::IOWrite, NULL, NULL, this, NULL);

	m_slot = uSlot;
}

//===========================================================================

BYTE __stdcall Disk2InterfaceCard::IORead(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	UINT uSlot = ((addr & 0xff) >> 4) - 8;
	Disk2InterfaceCard* pCard = (Disk2InterfaceCard*) MemGetSlotParameters(uSlot);

	switch (addr & 0xF)
	{
	case 0x0:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x1:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x2:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x3:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x4:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x5:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x6:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x7:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x8:	pCard->ControlMotor(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x9:	pCard->ControlMotor(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xA:	pCard->Enable(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xB:	pCard->Enable(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xC:	pCard->ReadWrite(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xD:	pCard->LoadWriteProtect(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xE:	pCard->SetReadMode(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xF:	pCard->SetWriteMode(pc, addr, bWrite, d, nExecutedCycles); break;
	}

	// only even addresses return the latch (UTAIIe Table 9.1)
	if (!(addr & 1))
		return pCard->m_floppyLatch;
	else
		return MemReadFloatingBus(nExecutedCycles);
}

BYTE __stdcall Disk2InterfaceCard::IOWrite(WORD pc, WORD addr, BYTE bWrite, BYTE d, ULONG nExecutedCycles)
{
	UINT uSlot = ((addr & 0xff) >> 4) - 8;
	Disk2InterfaceCard* pCard = (Disk2InterfaceCard*) MemGetSlotParameters(uSlot);

	switch (addr & 0xF)
	{
	case 0x0:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x1:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x2:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x3:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x4:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x5:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x6:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x7:	pCard->ControlStepper(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x8:	pCard->ControlMotor(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0x9:	pCard->ControlMotor(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xA:	pCard->Enable(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xB:	pCard->Enable(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xC:	pCard->ReadWrite(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xD:	pCard->LoadWriteProtect(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xE:	pCard->SetReadMode(pc, addr, bWrite, d, nExecutedCycles); break;
	case 0xF:	pCard->SetWriteMode(pc, addr, bWrite, d, nExecutedCycles); break;
	}

	// any address writes the latch via sequencer LD command (74LS323 datasheet)
	if (pCard->m_floppyWriteMode /* && m_floppyLoadMode */)
	{
		pCard->m_floppyLatch = d;
	}
	return 0;
}

//===========================================================================

// Unit version history:
// 2: Added: Format Track state & DiskLastCycle
// 3: Added: DiskLastReadLatchCycle
static const UINT kUNIT_VERSION = 3;

#define SS_YAML_VALUE_CARD_DISK2 "Disk]["

#define SS_YAML_KEY_PHASES "Phases"
#define SS_YAML_KEY_CURRENT_DRIVE "Current Drive"
#define SS_YAML_KEY_DISK_ACCESSED "Disk Accessed"
#define SS_YAML_KEY_ENHANCE_DISK "Enhance Disk"
#define SS_YAML_KEY_FLOPPY_LATCH "Floppy Latch"
#define SS_YAML_KEY_FLOPPY_MOTOR_ON "Floppy Motor On"
#define SS_YAML_KEY_FLOPPY_WRITE_MODE "Floppy Write Mode"
#define SS_YAML_KEY_LAST_CYCLE "Last Cycle"
#define SS_YAML_KEY_LAST_READ_LATCH_CYCLE "Last Read Latch Cycle"

#define SS_YAML_KEY_DISK2UNIT "Unit"
#define SS_YAML_KEY_FILENAME "Filename"
#define SS_YAML_KEY_TRACK "Track"
#define SS_YAML_KEY_PHASE "Phase"
#define SS_YAML_KEY_BYTE "Byte"
#define SS_YAML_KEY_WRITE_PROTECTED "Write Protected"
#define SS_YAML_KEY_SPINNING "Spinning"
#define SS_YAML_KEY_WRITE_LIGHT "Write Light"
#define SS_YAML_KEY_NIBBLES "Nibbles"
#define SS_YAML_KEY_TRACK_IMAGE_DATA "Track Image Data"
#define SS_YAML_KEY_TRACK_IMAGE_DIRTY "Track Image Dirty"
#define SS_YAML_KEY_TRACK_IMAGE "Track Image"

std::string Disk2InterfaceCard::GetSnapshotCardName(void)
{
	static const std::string name(SS_YAML_VALUE_CARD_DISK2);
	return name;
}

void Disk2InterfaceCard::SaveSnapshotDisk2Unit(YamlSaveHelper& yamlSaveHelper, UINT unit)
{
	YamlSaveHelper::Label label(yamlSaveHelper, "%s%d:\n", SS_YAML_KEY_DISK2UNIT, unit);
	yamlSaveHelper.SaveString(SS_YAML_KEY_FILENAME, m_floppyDrive[unit].m_disk.m_fullname);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_TRACK, m_floppyDrive[unit].m_track);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_PHASE, m_floppyDrive[unit].m_phase);
	yamlSaveHelper.SaveHexUint16(SS_YAML_KEY_BYTE, m_floppyDrive[unit].m_disk.m_byte);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_WRITE_PROTECTED, m_floppyDrive[unit].m_disk.m_bWriteProtected);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_SPINNING, m_floppyDrive[unit].m_spinning);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_WRITE_LIGHT, m_floppyDrive[unit].m_writelight);
	yamlSaveHelper.SaveHexUint16(SS_YAML_KEY_NIBBLES, m_floppyDrive[unit].m_disk.m_nibbles);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_TRACK_IMAGE_DATA, m_floppyDrive[unit].m_disk.m_trackimagedata);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_TRACK_IMAGE_DIRTY, m_floppyDrive[unit].m_disk.m_trackimagedirty);

	if (m_floppyDrive[unit].m_disk.m_trackimage)
	{
		YamlSaveHelper::Label image(yamlSaveHelper, "%s:\n", SS_YAML_KEY_TRACK_IMAGE);
		yamlSaveHelper.SaveMemory(m_floppyDrive[unit].m_disk.m_trackimage, NIBBLES_PER_TRACK);
	}
}

void Disk2InterfaceCard::SaveSnapshot(class YamlSaveHelper& yamlSaveHelper)
{
	YamlSaveHelper::Slot slot(yamlSaveHelper, GetSnapshotCardName(), m_slot, kUNIT_VERSION);

	YamlSaveHelper::Label state(yamlSaveHelper, "%s:\n", SS_YAML_KEY_STATE);
	yamlSaveHelper.SaveHexUint4(SS_YAML_KEY_PHASES, m_phases);
	yamlSaveHelper.SaveUint(SS_YAML_KEY_CURRENT_DRIVE, m_currDrive);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_DISK_ACCESSED, false);	// deprecated
	yamlSaveHelper.SaveBool(SS_YAML_KEY_ENHANCE_DISK, m_enhanceDisk);
	yamlSaveHelper.SaveHexUint8(SS_YAML_KEY_FLOPPY_LATCH, m_floppyLatch);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_FLOPPY_MOTOR_ON, m_floppyMotorOn == TRUE);
	yamlSaveHelper.SaveBool(SS_YAML_KEY_FLOPPY_WRITE_MODE, m_floppyWriteMode == TRUE);
	yamlSaveHelper.SaveHexUint64(SS_YAML_KEY_LAST_CYCLE, m_diskLastCycle);	// v2
	yamlSaveHelper.SaveHexUint64(SS_YAML_KEY_LAST_READ_LATCH_CYCLE, m_diskLastReadLatchCycle);	// v3
	m_formatTrack.SaveSnapshot(yamlSaveHelper);	// v2

	SaveSnapshotDisk2Unit(yamlSaveHelper, DRIVE_1);
	SaveSnapshotDisk2Unit(yamlSaveHelper, DRIVE_2);
}

void Disk2InterfaceCard::LoadSnapshotDriveUnit(YamlLoadHelper& yamlLoadHelper, UINT unit)
{
	std::string disk2UnitName = std::string(SS_YAML_KEY_DISK2UNIT) + (unit == DRIVE_1 ? std::string("0") : std::string("1"));
	if (!yamlLoadHelper.GetSubMap(disk2UnitName))
		throw std::string("Card: Expected key: ") + disk2UnitName;

	bool bImageError = false;

	m_floppyDrive[unit].m_disk.m_fullname[0] = 0;
	m_floppyDrive[unit].m_disk.m_imagename[0] = 0;
	m_floppyDrive[unit].m_disk.m_bWriteProtected = false;	// Default to false (until image is successfully loaded below)

	std::string filename = yamlLoadHelper.LoadString(SS_YAML_KEY_FILENAME);
	if (!filename.empty())
	{
		DWORD dwAttributes = GetFileAttributes(filename.c_str());
		if(dwAttributes == INVALID_FILE_ATTRIBUTES)
		{
			// Get user to browse for file
			UserSelectNewDiskImage(unit, filename.c_str());

			dwAttributes = GetFileAttributes(filename.c_str());
		}

		bImageError = (dwAttributes == INVALID_FILE_ATTRIBUTES);
		if (!bImageError)
		{
			if(InsertDisk(unit, filename.c_str(), dwAttributes & FILE_ATTRIBUTE_READONLY, IMAGE_DONT_CREATE) != eIMAGE_ERROR_NONE)
				bImageError = true;

			// DiskInsert() zeros m_floppyDrive[unit], then sets up:
			// . imagename
			// . fullname
			// . writeprotected
		}
	}

	m_floppyDrive[unit].m_track			= yamlLoadHelper.LoadUint(SS_YAML_KEY_TRACK);
	m_floppyDrive[unit].m_phase			= yamlLoadHelper.LoadUint(SS_YAML_KEY_PHASE);
	m_floppyDrive[unit].m_disk.m_byte	= yamlLoadHelper.LoadUint(SS_YAML_KEY_BYTE);
	yamlLoadHelper.LoadBool(SS_YAML_KEY_WRITE_PROTECTED);	// Consume
	m_floppyDrive[unit].m_spinning		= yamlLoadHelper.LoadUint(SS_YAML_KEY_SPINNING);
	m_floppyDrive[unit].m_writelight	= yamlLoadHelper.LoadUint(SS_YAML_KEY_WRITE_LIGHT);
	m_floppyDrive[unit].m_disk.m_nibbles			= yamlLoadHelper.LoadUint(SS_YAML_KEY_NIBBLES);
	m_floppyDrive[unit].m_disk.m_trackimagedata		= yamlLoadHelper.LoadUint(SS_YAML_KEY_TRACK_IMAGE_DATA) ? true : false;
	m_floppyDrive[unit].m_disk.m_trackimagedirty	= yamlLoadHelper.LoadUint(SS_YAML_KEY_TRACK_IMAGE_DIRTY) ? true : false;

	std::vector<BYTE> track(NIBBLES_PER_TRACK);
	if (yamlLoadHelper.GetSubMap(SS_YAML_KEY_TRACK_IMAGE))
	{
		yamlLoadHelper.LoadMemory(&track[0], NIBBLES_PER_TRACK);
		yamlLoadHelper.PopMap();
	}

	yamlLoadHelper.PopMap();

	//

	if (!filename.empty() && !bImageError)
	{
		if ((m_floppyDrive[unit].m_disk.m_trackimage == NULL) && m_floppyDrive[unit].m_disk.m_nibbles)
			AllocTrack(unit);

		if (m_floppyDrive[unit].m_disk.m_trackimage == NULL)
			bImageError = true;
		else
			memcpy(m_floppyDrive[unit].m_disk.m_trackimage, &track[0], NIBBLES_PER_TRACK);
	}

	if (bImageError)
	{
		m_floppyDrive[unit].m_disk.m_trackimagedata	= false;
		m_floppyDrive[unit].m_disk.m_trackimagedirty	= false;
		m_floppyDrive[unit].m_disk.m_nibbles			= 0;
	}
}

bool Disk2InterfaceCard::LoadSnapshot(class YamlLoadHelper& yamlLoadHelper, UINT slot, UINT version)
{
	if (slot != 6)	// fixme
		throw std::string("Card: wrong slot");

	if (version < 1 || version > kUNIT_VERSION)
		throw std::string("Card: wrong version");

	m_phases  			= yamlLoadHelper.LoadUint(SS_YAML_KEY_PHASES);
	m_currDrive			= yamlLoadHelper.LoadUint(SS_YAML_KEY_CURRENT_DRIVE);
	(void)				  yamlLoadHelper.LoadBool(SS_YAML_KEY_DISK_ACCESSED);	// deprecated - but retrieve the value to avoid the "State: Unknown key (Disk Accessed)" warning
	m_enhanceDisk		= yamlLoadHelper.LoadBool(SS_YAML_KEY_ENHANCE_DISK);
	m_floppyLatch		= yamlLoadHelper.LoadUint(SS_YAML_KEY_FLOPPY_LATCH);
	m_floppyMotorOn		= yamlLoadHelper.LoadBool(SS_YAML_KEY_FLOPPY_MOTOR_ON);
	m_floppyWriteMode	= yamlLoadHelper.LoadBool(SS_YAML_KEY_FLOPPY_WRITE_MODE);

	if (version >= 2)
	{
		m_diskLastCycle = yamlLoadHelper.LoadUint64(SS_YAML_KEY_LAST_CYCLE);
		m_formatTrack.LoadSnapshot(yamlLoadHelper);
	}

	if (version >= 3)
	{
		m_diskLastReadLatchCycle = yamlLoadHelper.LoadUint64(SS_YAML_KEY_LAST_READ_LATCH_CYCLE);
	}

	// Eject all disks first in case Drive-2 contains disk to be inserted into Drive-1
	for (UINT i=0; i<NUM_DRIVES; i++)
	{
		EjectDisk(i);	// Remove any disk & update Registry to reflect empty drive
		m_floppyDrive[i].clear();
	}

	LoadSnapshotDriveUnit(yamlLoadHelper, DRIVE_1);
	LoadSnapshotDriveUnit(yamlLoadHelper, DRIVE_2);

	FrameRefreshStatus(DRAW_LEDS | DRAW_BUTTON_DRIVES);

	return true;
}
