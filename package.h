/*DDK*************************************************************************/
/*                                                                           */
/* COPYRIGHT    Copyright (C) 1991, 2003 IBM Corporation                     */
/*                                                                           */
/*    The following IBM OS/2 source code is provided to you solely for       */
/*    the purpose of assisting you in your development of OS/2 device        */
/*    drivers. You may use this code in accordance with the IBM License      */
/*    Agreement provided in the IBM Developer Connection Device Driver       */
/*    Source Kit for OS/2. This Copyright statement may not be removed.      */
/*                                                                           */
/*****************************************************************************/
/**************************************************************************
 *
 * SOURCE FILE NAME = package.h
 *
 * DESCRIPTIVE NAME = package format definitions
 *
 *
 * VERSION = V2.0
 *
 * DATE
 *
 * DESCRIPTION : declarations of PAK structures, manipulation functions
 *
 * FUNCTIONS
 *
 * NOTES
 *
 * STRUCTURES
 *
 * EXTERNAL REFERENCES
 *
 * EXTERNAL FUNCTIONS
 *
*/
#pragma pack(1)
#ifndef package_h_
#define package_h_

//
// IMPORTANT!
// Do not modify existing package formats.
// Only add new, only if you really really need to.
//



//
//  PAK file Header structure . This structure contains PPB file identifier
//  Table size and number of entries in the PPB file .
//

typedef struct _PAKSIGNATURE
{
  CHAR  szName[40];
  ULONG ulCRC; // not used in V1. should check integrity of entry directory, at least.
  SHORT iTblSize;
  SHORT iEntries;
  CHAR  free[4];
} PAKSIGNATURE, *PPAKSIGNATURE;

//
// Various signatures we that are out there
//
#define PAKSIGNATURE_DEVPACK_V1   "IBM DDPAK V1.2"  // ALT_CUPS
#define PAKSIGNATURE_FONTPACK_V1  "IBM FONTS V1.0"

//
// These are names of packages that driver recognizes
// Must be consistent with makefile and PIN hardwired values
//
#define PAKNAME_IBMDEV_PACK   "printer1.pak"
#define PAKNAME_AUXDEV_PACK   "auxprint.pak"

#define PAKNAME_IBMFONT_PACK  "font1.pak"
#define PAKNAME_AUXFONT_PACK  "auxfont.pak"

//
// Various directory entry types (indicated by PAKSIGNATURE)
//

// printer device package
typedef struct _PAK_DEV_DIRENTRY
{
  CHAR szDeviceName[40];
  ULONG ulOffset;
  ULONG ulSize;
  ULONG ulFlags;
  CHAR free[4];
} PAK_DEV_DIRENTRY, *PPAK_DEV_DIRENTRY;


// device font package
typedef struct _PAK_FONT_DIRENTRY
{
  CHAR szFontName[40];
  CHAR szFontFullName[40];
  ULONG ulOffset;
  ULONG ulSize;
  ULONG ulFlags;
  CHAR free[4];
} PAK_FONT_DIRENTRY, *PPAK_FONT_DIRENTRY;


//
// Functions
//
PBYTE LoadPakDirectory( PVOID pHeap, PSZ pszPakFile, PSZ pszExpectedSignature, ULONG ulSegmentSize );

PPAK_FONT_DIRENTRY GetFontDirEntry( PSZ pszFontName, PBYTE pDirectoryMem );
PPAK_FONT_DIRENTRY GetFontDirEntryByFullName( PSZ pszFontFullName, PBYTE pDirectoryMem );
PPAK_DEV_DIRENTRY GetDeviceDirEntry( PSZ pszDeviceName, PBYTE pDirectoryMem);

PBYTE LoadPakDeviceSegment ( PVOID pHeap, PSZ pszPakFile, PBYTE pDirectoryMem, PSZ pszDeviceName);
PBYTE LoadPakFontSegment ( PVOID pHeap, PSZ pszPakFile, PBYTE pDirectoryMem, PSZ pszFontName);


#endif
