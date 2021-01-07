/*
 * Syntax: ppaktool <pakfile> [<action>]
 *
 * Possible values for <action>:
 *    l              List printers defined in <pakfile>
 *    v "<printer>"  View data for <printer>, formatted by structure
 *    r "<printer>"  View data for <printer>, formatted for readability
 *    p "<printer>"  Generate a PIN-compatible PPD file for <printer> (to stdout)
 *    d "<printer>"  Dump the raw (binary) data for <printer> (to stdout)
 *    x "<printer>"  Dump the hexadecimal (binary) data for <printer>
 *    b "<printer>"  Dump the (binary) data for <printer> in prettified hex/raw comparison
 */

#define INCL_DOSFILEMGR
#define INCL_DOSERRORS
#include <os2.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "pt_struct.h"
#include "package.h"
#include "ppdtable.h"

// Overrides signature definition in package.h
#if PSDRIVER == 1
#define PAKSIGNATURE_DEVPACK_V1   "IBM DDPAK V1.2"  // ALT_CUPS
#else
#define PAKSIGNATURE_DEVPACK_V1   "IBM DDPAK V1.0"  // ALT_CUPS
#endif

// These correspond to the program execution modes (according to cmd-line)
#define ACTION_LIST  1      // list printers
#define ACTION_VIEW  2      // view structured data
#define ACTION_DUMP  3      // dump raw data
#define ACTION_HEX   4      // dump hex data
#define ACTION_BOTH  5      // dump both hex & raw data
#define ACTION_READ  6      // show readable data
#define ACTION_PPD   7      // generate PPD file

// Values for the data-format flag passed to ShowPrinterData()
#define DEV_FMT_DATA 1      // formatted (structured) data
#define DEV_RAW_DATA 2      // raw data dump
#define DEV_HEX_DATA 3      // hex data dump
#define DEV_BIN_DATA 4      // combined binary (raw/hex) data dump
#define DEV_TXT_DATA 5      // readable text
#define DEV_PPD_DATA 6      // PPD output

// Convert an offset into the data buffer into a pointer to string.  Note:
// i is the offset value; p is the pointer to the information segment buffer.
#define OFFSET_TO_PSZ(i, p)    ((i > 0)? (PSZ)(p+i): "(none)")

// Some useful macros for formatted output of offset values
#define OFFSET_FORMAT( i )     ((i <= 0)? "  %7d": "%#9x")

// Replace certain undisplayable raw-byte values
#define DISPLAYABLE_CHAR( c )  ( c == 0 ? ' ' : ( c < 32 ? 127: c ))


ULONG  ListPrinters( PSZ pszPakFile );
ULONG  ShowPrinterData( PSZ pszPakFile, PSZ pszPrinter, USHORT fsMode );
void   ShowDeviceData( PAK_DEV_DIRENTRY pdd, PBYTE pBuf );
void   ShowReadableData( PAK_DEV_DIRENTRY pdd, PBYTE pBuf );
void   GeneratePPD( PBYTE pBuf );
void   DumpBytes( PBYTE pBuf, ULONG cb, BOOL fHex );
void   PrettyBytes( PBYTE pBuf, ULONG cb );
PSZ    OffsetToCommand( SHORT sOff, PBYTE pIn, PBYTE pOut );
USHORT DecompressString(PSZ pszBuffIn, PSZ pszBuffOut);


/* ------------------------------------------------------------------------- */
int main( int argc, char *argv[] )
{
    PSZ    pszPakFile = PAKNAME_AUXDEV_PACK,
           pszArg     = NULL;
    USHORT usAction   = ACTION_LIST;
    APIRET rc = 0;

    if ( argc > 1 ) {
        pszPakFile = argv[1];
        if ( argc > 2 ) {
            strupr( argv[2] );
            if ( *argv[2] == '/' || *argv[2] == '-') argv[2]++;
            switch ( *argv[2] ) {
                case 'L':  usAction = ACTION_LIST; break;
                case 'V':  usAction = ACTION_VIEW; break;
                case 'R':  usAction = ACTION_READ; break;
                case 'D':  usAction = ACTION_DUMP; break;
                case 'X':  usAction = ACTION_HEX;  break;
                case 'B':  usAction = ACTION_BOTH; break;
                case 'P':  usAction = ACTION_PPD;  break;
            }
            if ( argc > 3 ) pszArg = argv[3];
        }
    }
    else {
        printf("PostScript PAK Utility version 0.2\n");
        printf("Syntax: ppaktool <pakfile> [<action>]\n\n");
        printf("Supported actions:\n\n");
        printf(" L              List printers in driver PAK file <pakfile> (default)\n\n");
        printf(" P \"<printer>\"  Generate a PIN-compatible PPD listing for <printer>\n");
        printf(" R \"<printer>\"  View data for <printer> in a form optimized for readability\n");
        printf(" V \"<printer>\"  View data for <printer>, formatted by its internal structure\n\n");
        printf(" B \"<printer>\"  Dump binary data for <printer> in combined (raw/hex) format\n");
        printf(" D \"<printer>\"  Dump binary data for <printer> as raw bytes\n");
        printf(" X \"<printer>\"  Dump binary data for <printer> as hexadecimal bytes\n\n");
        printf("All output is to STDOUT.\n");
        return 0;
    }

    switch ( usAction ) {
        case ACTION_LIST : rc = ListPrinters( pszPakFile );                          break;
        case ACTION_VIEW : rc = ShowPrinterData( pszPakFile, pszArg, DEV_FMT_DATA ); break;
        case ACTION_READ : rc = ShowPrinterData( pszPakFile, pszArg, DEV_TXT_DATA ); break;
        case ACTION_PPD  : rc = ShowPrinterData( pszPakFile, pszArg, DEV_PPD_DATA ); break;
        case ACTION_DUMP : rc = ShowPrinterData( pszPakFile, pszArg, DEV_RAW_DATA ); break;
        case ACTION_HEX  : rc = ShowPrinterData( pszPakFile, pszArg, DEV_HEX_DATA ); break;
        case ACTION_BOTH : rc = ShowPrinterData( pszPakFile, pszArg, DEV_BIN_DATA ); break;
    }

    if ( rc ) printf("Error reading file (error %u)\n", rc );
    return rc;
}


/* ------------------------------------------------------------------------- */
ULONG ListPrinters( PSZ pszPakFile )
{
    HFILE            hf;
    ULONG            ulResult;
    SHORT            i;
    PAKSIGNATURE     pak_sig;
    PAK_DEV_DIRENTRY pak_dev;
    APIRET           rc;

    if (( rc = DosOpen( pszPakFile, &hf, &ulResult, 0, 0,
                        OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                        OPEN_FLAGS_FAIL_ON_ERROR | OPEN_FLAGS_SEQUENTIAL |
                        OPEN_SHARE_DENYNONE | OPEN_ACCESS_READONLY, NULL )) > 0 )
    {
        switch ( rc ) {
            case 2: printf("The file \"pszPakFile\" was not found.\n");
                    break;
            case 3: printf("The specified path was not found.\n");
                    break;
            case 5: printf("Access denied.\n");
                    break;
        }
        return rc;
    }

    rc = DosRead( hf, &pak_sig, sizeof( PAKSIGNATURE ), &ulResult );
    if ( rc ) goto cleanup;
    if (( ulResult < sizeof( PAKSIGNATURE )) ||
        ( strcmp( pak_sig.szName, PAKSIGNATURE_DEVPACK_V1 ) != 0 ))
    {
        printf("Invalid PAK file signature!\n");
        if ( ! strncmp( pak_sig.szName, PAKSIGNATURE_DEVPACK_V1, 9 )) {
            printf(" - Expected signature: %s\n", PAKSIGNATURE_DEVPACK_V1 );
            printf(" - Found signature:    %s\n", pak_sig.szName );
            printf("This PAK file seems to have been created for a different printer driver.\n");
        }
        rc = ERROR_INVALID_DATA;
        goto cleanup;
    }

    printf("%s\n==============\n", pak_sig.szName );
    printf("%d printers defined:\n", pak_sig.iEntries );

    for ( i = 0; i < pak_sig.iEntries; i++ ) {
        rc = DosRead( hf, &pak_dev, sizeof( PAK_DEV_DIRENTRY ), &ulResult );
        if ( rc || ulResult < sizeof( PAK_DEV_DIRENTRY )) goto cleanup;
        printf(" - %s (offset 0x%X, %u bytes, flags=0x%X)\n",
                pak_dev.szDeviceName, pak_dev.ulOffset, pak_dev.ulSize, pak_dev.ulFlags );
    }

cleanup:
    DosClose( hf );
    return rc;
}


/* ------------------------------------------------------------------------- */
ULONG ShowPrinterData( PSZ pszPakFile, PSZ pszPrinter, USHORT fsMode )
{
    HFILE            hf;
    ULONG            ulResult;
    SHORT            i;
    BOOL             fFound = FALSE;
    PAKSIGNATURE     pak_sig;
    PAK_DEV_DIRENTRY pak_dev;
    PBYTE            pBuf;
    APIRET           rc;

    if (( rc = DosOpen( pszPakFile, &hf, &ulResult, 0, 0,
                        OPEN_ACTION_FAIL_IF_NEW | OPEN_ACTION_OPEN_IF_EXISTS,
                        OPEN_FLAGS_FAIL_ON_ERROR | OPEN_FLAGS_SEQUENTIAL |
                        OPEN_SHARE_DENYNONE | OPEN_ACCESS_READONLY, NULL )) > 0 )
    {
        switch ( rc ) {
            case 2: printf("The file \"pszPakFile\" was not found.\n");
                    break;
            case 3: printf("The specified path was not found.\n");
                    break;
            case 5: printf("Access denied.\n");
                    break;
        }
        return rc;
    }

    rc = DosRead( hf, &pak_sig, sizeof( PAKSIGNATURE ), &ulResult );
    if ( rc ) goto cleanup;
    if (( ulResult < sizeof( PAKSIGNATURE )) ||
        ( strcmp( pak_sig.szName, PAKSIGNATURE_DEVPACK_V1 ) != 0 ))
    {
        printf("Invalid PAK file signature!\n");
        rc = ERROR_INVALID_DATA;
        goto cleanup;
    }

    // If no printer name was requested, just grab the first one
    if ( !pszPrinter ) {
        rc = DosRead( hf, &pak_dev, sizeof( PAK_DEV_DIRENTRY ), &ulResult );
        if ( rc || ulResult < sizeof( PAK_DEV_DIRENTRY )) goto cleanup;
        fFound = TRUE;
    }
    // Otherwise, search the directory for the specified printer
    else for ( i = 0; i < pak_sig.iEntries; i++ ) {
        rc = DosRead( hf, &pak_dev, sizeof( PAK_DEV_DIRENTRY ), &ulResult );
        if ( rc || ulResult < sizeof( PAK_DEV_DIRENTRY )) goto cleanup;
        if ( stricmp( pak_dev.szDeviceName, pszPrinter ) == 0 ) {
            fFound = TRUE;
            break;
        }
    }
    if ( !fFound ) {
        printf("The requested printer was not found\n");
        goto cleanup;
    }

    // Seek to the indicated offset for this printer's data
    rc = DosSetFilePtr( hf, pak_dev.ulOffset, FILE_BEGIN, &ulResult );
    if ( rc || ulResult != pak_dev.ulOffset ) goto cleanup;

    // Read the data into a simple buffer
    if (( pBuf = (PBYTE) malloc( pak_dev.ulSize )) == NULL ) {
        printf("malloc() failed - out of memory?\n");
        goto cleanup;
    }
    rc = DosRead( hf, pBuf, pak_dev.ulSize, &ulResult );
    if ( rc || ulResult < pak_dev.ulSize ) {
        printf("Error reading device data.\n");
        free( pBuf );
        goto cleanup;
    }

    // OK, we have the data... now output it in the manner requested.
    switch ( fsMode ) {
        case DEV_FMT_DATA: ShowDeviceData( pak_dev, pBuf );          break;
        case DEV_TXT_DATA: ShowReadableData( pak_dev, pBuf );        break;
        case DEV_PPD_DATA: GeneratePPD( pBuf );                      break;
        case DEV_RAW_DATA: DumpBytes( pBuf, pak_dev.ulSize, FALSE ); break;
        case DEV_HEX_DATA: DumpBytes( pBuf, pak_dev.ulSize, TRUE );  break;
        case DEV_BIN_DATA: PrettyBytes( pBuf, pak_dev.ulSize );      break;
    }

    free( pBuf );

cleanup:
    DosClose( hf );
    return rc;
}


/* ------------------------------------------------------------------------- *
 * DumpBytes                                                                 *
 *                                                                           *
 * Dump the contents of a buffer to STDOUT as either raw or hex byte values. *
 *                                                                           *
 * PARAMETERS:                                                               *
 *   PBYTE pBuf: Pointer to the data being dumped                            *
 *   ULONG cb  : Number of bytes to dump                                     *
 *   BOOL  fHex: Output as hex values instead of literal bytes?              *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void DumpBytes( PBYTE pBuf, ULONG cb, BOOL fHex )
{
    ULONG i;
    SHORT sCol = 0;

    for ( i = 0; i < cb; i++ ) {
        if ( fHex ) {
            printf("%02X", *(pBuf+i) );
            sCol += 3;
            if ( sCol > 75 ) {
                printf("\n");
                sCol = 0;
            } else
                printf(" ");
        }
        else printf("%c", *(pBuf+i) );
    }
}


/* ------------------------------------------------------------------------- *
 * PrettyBytes                                                               *
 *                                                                           *
 * Dump the contents of a buffer to STDOUT in a nice-looking table with both *
 * hexadecimal and literal (character) values displayed side-by-side.        *
 *                                                                           *
 * PARAMETERS:                                                               *
 *   PBYTE pBuf: Pointer to the data being dumped                            *
 *   ULONG cb  : Number of bytes to dump                                     *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PrettyBytes( PBYTE pBuf, ULONG cb )
{
    ULONG i, j, k;
    USHORT count = 0;

    printf("     +------------------------------------------------+----------------+\n");
    printf("     |+0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F |0123456789ABCDEF|\n");
    printf("+----+------------------------------------------------+----------------+\n");
    printf("|0000|");
    for ( i = 0, j = 0; i < cb; i++ ) {
        printf("%02X", *(pBuf+i) );
        count++;
        if ( count > 15 ) {
            printf(" |");
            for ( ; j <= i; j++ ) printf("%c", DISPLAYABLE_CHAR( *(pBuf+j) ));
            printf("|\n|%04X|", i+1 );
            count = 0;
        } else
            printf(" ");
    }
    if ( j < i ) {
        for ( k = (i%16); k < 16; k++ ) printf("   ");
        printf("|");
        for ( ; j <= i; j++ ) printf("%c", DISPLAYABLE_CHAR( *(pBuf+j) ));
        for ( k = (j%16); k < 16; k++ ) printf(" ");
        printf("|\n");
    }
    printf("+----+------------------------------------------------+----------------+\n");
    printf("     |+0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F |0123456789ABCDEF|\n");
    printf("     +------------------------------------------------+----------------+\n");
}


/* ------------------------------------------------------------------------- */
void print_offcell( PSZ pszName, SHORT sValue, BOOL fNL )
{
    printf("%-24s = ", pszName);
    printf( OFFSET_FORMAT(sValue), sValue );
    if ( fNL )
        printf(" |\n| ");
    else
        printf(" | ");
}


/* ------------------------------------------------------------------------- */
void ShowDeviceData( PAK_DEV_DIRENTRY pdd, PBYTE pBuf )
{
    DESPPD     desPPD = {0};
    PBYTE      pInfoSeg;
    PUI_BLOCK  puib;
    PUIC_BLOCK puicb;
    ULONG      ulCB,
               cbDS,
               cbIS;
    USHORT     i, j;
    PSHORT     psRes;


    cbDS = sizeof( DESPPD );
    memcpy( (PBYTE) &desPPD, pBuf, cbDS );
    pInfoSeg = pBuf + cbDS;

    // Allocate and copy the dynamic data items in DESPPD
    ulCB = desPPD.stUIList.usBlockListSize;
    desPPD.stUIList.pBlockList = (PUI_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUIList.pBlockList, (PUI_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;
    cbDS += ulCB;

    ulCB = desPPD.stUICList.usNumOfUICs * sizeof( UIC_BLOCK );
    desPPD.stUICList.puicBlockList = (PUIC_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUICList.puicBlockList, (PUIC_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;
    cbDS += ulCB;
    cbIS = pdd.ulSize - cbDS;
    desPPD.pPSStringBuff = pInfoSeg;

    // Print header
    printf("/=============================================================================\\\n");
    printf("| PRINTER PAK ENTRY                                                           |\n");
    printf("| %-75s |\n", pdd.szDeviceName );
    printf("+-----------------------------------------------------------------------------+\n");
    printf("| %5u bytes total                                                           |\n", pdd.ulSize );
    printf("|  - %5u bytes in descriptor segment                                        |\n", cbDS );
    printf("|  - %5u bytes in information segment                                       |\n", cbIS );
    printf("\\=============================================================================/\n\n");

    printf("\n-------------------------------------------------------------------------------");
    printf("\n                         DESCRIPTOR SEGMENT (DESPPD)                           ");
    printf("\n-------------------------------------------------------------------------------\n");

    // Print the structured data from DESPPD
    // PPD1
    printf("+-----------------+\n");
    printf("| desItems (PPD1) |\n");
    printf("+-----------------+--------------------+--------------------------------------+\n| ");
    printf("iSizeBuffer              =   %7d | ", desPPD.desItems.iSizeBuffer );
    print_offcell("ofsExitserver",  desPPD.desItems.ofsExitserver, TRUE );
    print_offcell("ofsPswrd",       desPPD.desItems.ofsPswrd, FALSE );
    printf("iScreenAngle             =   %7d |\n| ", desPPD.desItems.iScreenAngle );
    printf("iPpm                     =   %7d | ", desPPD.desItems.iPpm );
    printf("usLanguageLevel          =   %7d |\n| ", desPPD.desItems.usLanguageLevel );
    printf("lFreeVM                  = %9d | ", desPPD.desItems.lFreeVM );
    print_offcell("ofsTransferNor", desPPD.desItems.ofsTransferNor, TRUE );
    print_offcell("ofsPrType",      desPPD.desItems.ofsPrType, FALSE );
    print_offcell("ofsTransferInv", desPPD.desItems.ofsTransferInv, TRUE );
    print_offcell("ofsPrName",      desPPD.desItems.ofsPrName, FALSE );
    print_offcell("ofsInitString",  desPPD.desItems.ofsInitString, TRUE );
    printf("iResDpi                  =   %7d | ", desPPD.desItems.iResDpi );
    print_offcell("ofsJCLToPS",     desPPD.desItems.ofsJCLToPS, TRUE );
    printf("ResList.uNumOfRes        =   %7d | ", desPPD.desItems.ResList.uNumOfRes );
    print_offcell("ofsTermString",  desPPD.desItems.ofsTermString, TRUE );
    printf("ResList.uResOffset       =   %7d | ", desPPD.desItems.ResList.uResOffset );
    printf("sDefaultDuplex           =   %7d |\n| ", desPPD.desItems.sDefaultDuplex );
    printf("ResList.bIsJCLResolution =   %7d | ", desPPD.desItems.ResList.bIsJCLResolution );
    // Note: the duplex options appear to be unused and will thus be 0 (not -1)
    printf("ofsDuplexFalse           =   %7d |\n| ", desPPD.desItems.ofsDuplexFalse );
    printf("lScrFreq                 =   %7d | ", desPPD.desItems.lScrFreq );
    printf("ofsDuplexNoTumble        =   %7d |\n| ", desPPD.desItems.ofsDuplexNoTumble );
    printf("fIsColorDevice           =   %7d | ", desPPD.desItems.fIsColorDevice );
    printf("ofsDuplexTumble          =   %7d |\n| ", desPPD.desItems.ofsDuplexTumble );
    printf("fIsFileSystem            =   %7d | ", desPPD.desItems.fIsFileSystem );
    print_offcell("ofsPCFileName",  desPPD.desItems.ofsPCFileName, TRUE );
    print_offcell("ofsReset",       desPPD.desItems.ofsReset, FALSE );
#if PSDRIVER == 1
    printf("fTTSupport               =   %7d |\n", desPPD.desItems.fTTSupport );
#else
    printf("                                     |\n");
#endif
    printf("+--------------------------------------+--------------------------------------+\n\n");

    // PPD2
    printf("+----------------+\n");
    printf("| desPage (PPD2) |\n");
    printf("+----------------+---------------------+--------------------------------------+\n| ");
#if PSDRIVER > 2
    printf("                                     | ");
#else
    print_offcell("ofsDfpgsz",       desPPD.desPage.ofsDfpgsz, FALSE );
#endif
    printf("iImgpgpairs              =   %7d |\n| ", desPPD.desPage.iImgpgpairs );
    printf("fIsVariablePaper         =   %7d | ", desPPD.desPage.fIsVariablePaper );
    print_offcell("ofsImgblPgsz",    desPPD.desPage.ofsImgblPgsz, TRUE );
#if PSDRIVER > 2
    printf("                                     | ");
#else
    print_offcell("ofsDefimagearea", desPPD.desPage.ofsDefimagearea, FALSE );
#endif
    print_offcell("ofsCustomPageSize", desPPD.desPage.ofsCustomPageSize, TRUE );
#if PSDRIVER > 2
    printf("                                     | ");
#else
    print_offcell("ofsDefpaperdim",  desPPD.desPage.ofsDefpaperdim, FALSE );
#endif
    printf("iCustomPageSizeMinWidth  =   %7d |\n| ", desPPD.desPage.iCustomPageSizeMinWidth );
#if PSDRIVER > 2
    printf("                                     | ");
#else
    printf("iCmpgpairs               =   %7d | ", desPPD.desPage.iCmpgpairs );
#endif
    printf("iCustomPageSizeMaxWidth  =   %7d |\n| ", desPPD.desPage.iCustomPageSizeMaxWidth );
#if PSDRIVER > 2
    printf("                                     | ");
#else
    print_offcell("ofsLspgCmnds",    desPPD.desPage.ofsLspgCmnds, FALSE );
#endif
    printf("iCustomPageSizeMinHeight =   %7d |\n| ", desPPD.desPage.iCustomPageSizeMinHeight );
    printf("iDmpgpairs               =   %7d | ", desPPD.desPage.iDmpgpairs );
    printf("iCustomPageSizeMaxHeight =   %7d |\n| ", desPPD.desPage.iCustomPageSizeMaxHeight );
    print_offcell("ofsDimxyPgsz",    desPPD.desPage.ofsDimxyPgsz, FALSE );
#if PSDRIVER > 2
    printf("sReserved1               =   %7d |\n| ", desPPD.desPage.sReserved1 );
    printf("                                     | ");
    printf("sReserved2               =   %7d |\n", desPPD.desPage.sReserved2 );
#else
    printf("                                     |\n");
#endif
    printf("+--------------------------------------+--------------------------------------+\n\n");

    // PPD3
    printf("+-------------------+\n");
    printf("| desInpbins (PPD3) |\n");
    printf("+-------------------+------------------+--------------------------------------+\n| ");
    printf("iManualfeed              =   %7d | ", desPPD.desInpbins.iManualfeed );
    printf("iInpbinpairs             =   %7d |\n| ", desPPD.desInpbins.iInpbinpairs );
    print_offcell("ofsManualtrue",   desPPD.desInpbins.ofsManualtrue, FALSE );
    print_offcell("ofsCmInpbins",    desPPD.desInpbins.ofsCmInpbins, TRUE );
    print_offcell("ofsManualfalse",  desPPD.desInpbins.ofsManualfalse, FALSE );
    printf("iNumOfPageSizes          =   %7d |\n| ", desPPD.desInpbins.iNumOfPageSizes );
    print_offcell("ofsDefinputslot", desPPD.desInpbins.ofsDefinputslot, FALSE );
    print_offcell("ofsPageSizes",    desPPD.desInpbins.ofsPageSizes, FALSE );
    printf("\n+--------------------------------------+--------------------------------------+\n\n");

    // PPD4
    printf("+-------------------+\n");
    printf("| desOutbins (PPD4) |\n");
    printf("+-------------------+------------------+--------------------------------------+\n| ");
    printf("fIsDefoutorder           =   %7d | ", desPPD.desOutbins.fIsDefoutorder );
    print_offcell("ofsDefoutputbin", desPPD.desOutbins.ofsDefoutputbin, TRUE );
    print_offcell("ofsOrdernormal",  desPPD.desOutbins.ofsOrdernormal, FALSE );
    printf("iOutbinpairs             =   %7d |\n| ", desPPD.desOutbins.iOutbinpairs );
    print_offcell("ofsOrderreverse", desPPD.desOutbins.ofsOrderreverse, FALSE );
    print_offcell("ofsCmOutbins",    desPPD.desOutbins.ofsCmOutbins, FALSE );
    printf("\n+--------------------------------------+--------------------------------------+\n\n");

    // PPD5
    printf("+-----------------+\n");
    printf("| desFonts (PPD5) |\n");
    printf("+-----------------+--------------------+\n| ");
    print_offcell("ofsDeffont", desPPD.desFonts.ofsDeffont, TRUE );
    printf("iFonts                   =   %7d |\n| ", desPPD.desFonts.iFonts );
    print_offcell("ofsFontnames", desPPD.desFonts.ofsFontnames, FALSE );
    printf("\n+--------------------------------------+\n\n");

    // PPD6
    printf("+-----------------+\n");
    printf("| desForms (PPD6) |\n");
    printf("+-----------------+--------------------+\n| ");
    printf("usFormCount              =   %7d |\n| ", desPPD.desForms.usFormCount );
    print_offcell("ofsFormTable", desPPD.desForms.ofsFormTable, TRUE );
    print_offcell("ofsFormIndex", desPPD.desForms.ofsFormIndex, FALSE );
    printf("\n+--------------------------------------+\n\n");

    // UI_LIST
    printf("+--------------------+\n");
    printf("| stUIList (UI_LIST) |\n");
    printf("+--------------------+--------------------------------------------------------+\n");
    printf("| usNumOfBlocks       =   %7d                                             |\n", desPPD.stUIList.usNumOfBlocks );
    printf("| usBlockListSize     =   %7d                                             |\n", desPPD.stUIList.usBlockListSize );
    puib = desPPD.stUIList.pBlockList;
    for ( i = 0; puib && desPPD.stUIList.usBlockListSize && i < desPPD.stUIList.usNumOfBlocks; i++ ) {
        printf("+-----------------------------------------------------------------------------+\n");
        printf("| pBlockList[ %2d ]:                                                           |\n", i );
//                OFFSET_TO_PSZ(puib->ofsUITransString, pInfoSeg) );
//                OFFSET_TO_PSZ(puib->ofsUIName, pInfoSeg) );
        printf("|    ofsUIName        = ");
        printf( OFFSET_FORMAT((SHORT)(puib->ofsUIName)), puib->ofsUIName );
        printf("                                             |\n");
        printf("|    ofsUITransString = ");
        printf( OFFSET_FORMAT((SHORT)(puib->ofsUITransString)), puib->ofsUITransString );
        printf("                                             |\n");
        printf("|    usOrderDep       =   %7d                                             |\n", puib->usOrderDep );
        printf("|    usDisplayOrder   =   %7d                                             |\n", puib->usDisplayOrder );
        printf("|    usUILocation     =   %7d                                             |\n", puib->usUILocation );
        printf("|    usSelectType     =   %7d                                             |\n", puib->usSelectType );
        printf("|    ucGroupType      =   %7d                                             |\n", puib->ucGroupType );
        printf("|    ucPanelID        =   %7d                                             |\n", puib->ucPanelID    );
        printf("|    usDefaultEntry   =   %7d                                             |\n", puib->usDefaultEntry );
        printf("|    usNumOfEntries   =   %7d                                             |\n", puib->usNumOfEntries );
        printf("|    uiEntry[]................................................................|\n");
        for ( j = 0; j < puib->usNumOfEntries; j++ ) {
            printf("|    : %2d:  ofsOption = %#6x   ofsTransString = %#6x   ofsValue = %#6x :|\n",
                    j, puib->uiEntry[j].ofsOption, puib->uiEntry[j].ofsTransString, puib->uiEntry[j].ofsValue );
            // OFFSET_TO_PSZ(puib->uiEntry[j].ofsOption, pInfoSeg) );
            // OFFSET_TO_PSZ(puib->uiEntry[j].ofsTransString, pInfoSeg) );
        }
        printf("|    .........................................................................|\n");
        INCREMENT_BLOCK_PTR( puib );
    }
    printf("+-----------------------------------------------------------------------------+\n\n");

    // UIC_LIST
    printf("+----------------------+\n");
    printf("| stUICList (UIC_LIST) |\n");
    printf("+----------------------+------------------------------------------------------+\n");
    printf("| usNumOfUICs             =   %7d                                         |\n", desPPD.stUICList.usNumOfUICs );
    puicb = desPPD.stUICList.puicBlockList;
    for ( i = 0; puicb && i < desPPD.stUICList.usNumOfUICs; i++ ) {
        printf("+-----------------------------------------------------------------------------+\n");
        printf("| puicBlockList[ %2d ]:                                                        |\n", i );
        printf("|    uicEntry1.ofsUIBlock =   %7d                                         |\n", puicb->uicEntry1.ofsUIBlock );
        printf("|    uicEntry1.bOption    =%#10x                                         |\n", puicb->uicEntry1.bOption );
        printf("|    uicEntry2.ofsUIBlock =   %7d                                         |\n", puicb->uicEntry2.ofsUIBlock );
        printf("|    uicEntry2.bOption    =%#10x                                         |\n", puicb->uicEntry2.bOption );
        puicb++;
    }
    printf("+-----------------------------------------------------------------------------+\n\n");

    printf("\n-------------------------------------------------------------------------------");
    printf("\n                            INFORMATION SEGMENT                                ");
    printf("\n-------------------------------------------------------------------------------\n");
    PrettyBytes( pInfoSeg, cbIS );
    printf("\n");

    free( desPPD.stUIList.pBlockList );
    free( desPPD.stUICList.puicBlockList );
}


/* ------------------------------------------------------------------------- *
 * OffsetToCommand                                                           *
 *                                                                           *
 * Given an offset into the data buffer, convert the compressed command      *
 * string at that address into a normal, readable string.  Any newline chars *
 * will be stripped out in order to improve readability.  The string will be *
 * enclosed in quotes.                                                       *
 *                                                                           *
 * PARAMETERS:                                                               *
 *   SHORT sOff: Offset into the data buffer pIn                             *
 *   PBYTE pIn : Pointer to the input data buffer                            *
 *   PBYTE pOut: Pointer to a buffer into which the output string will be    *
 *               written (the buffer is assumed to be large enough)          *
 *                                                                           *
 * RETURNS: PSZ                                                              *
 *   Modified string (same as pOut)                                          *
 * ------------------------------------------------------------------------- */
PSZ OffsetToCommand( SHORT sOff, PBYTE pIn, PBYTE pOut )
{
    PSZ    pszTmp;
    USHORT usRC;

    if ( !pIn || !pOut || sOff < 1 )
        strcpy( pOut, "(none)");
    else {
        usRC = DecompressString( OFFSET_TO_PSZ( sOff, pIn ), pOut );
        if ( usRC ) {
            pszTmp = (PSZ) calloc( usRC + 3, sizeof( char ));
            if ( pszTmp ) {
                USHORT i, j;

                sprintf( pszTmp, "\"%s\"", (PSZ) pOut );
                usRC = strlen( pszTmp );
                for ( i = 0, j = 0; i < usRC; i++ ) {
                    if ( pszTmp[ i ] != '\r' && pszTmp[ i ] != '\n')
                        pOut[ j++ ] = pszTmp[ i ];
                }
                pOut[ j ] = 0;
                free( pszTmp );
            }
        }
        else if ( *((PSZ) pIn + sOff ))
            strcpy( pOut, (PSZ) pIn + sOff );
        else
            strcpy( pOut, "(none)");
    }
    return ( pOut );
}


/* ------------------------------------------------------------------------- *
 * OffsetToProperCommand                                                     *
 *                                                                           *
 * Given an offset into the data buffer, convert the compressed command      *
 * string at that address into a normal, readable string.  Any non-ASCII     *
 * characters will be replaced with hex strings.  The string is not quoted.  *
 * Basically, this function is used for the *JCL command strings, which may  *
 * contain odd byte values which won't be handled normally.  It also doesn't *
 * check that the offset is > 0, because for the first three JCL commands,   *
 * the correct offset could well BE 0 (as they appear, if they exist at all, *
 * right at the start of the data segment).                                  *
 *                                                                           *
 * PARAMETERS:                                                               *
 *   SHORT sOff: Offset into the data buffer pIn                             *
 *   PBYTE pIn : Pointer to the input data buffer                            *
 *   PBYTE pOut: Pointer to a buffer into which the output string will be    *
 *               written (the buffer is assumed to be large enough)          *
 *                                                                           *
 * RETURNS: PSZ                                                              *
 *   Modified string (same as pOut)                                          *
 * ------------------------------------------------------------------------- */
PSZ OffsetToProperCommand( SHORT sOff, PBYTE pIn, PBYTE pOut )
{
    PSZ    pszTmp;
    USHORT usRC;

    if ( !pIn || !pOut || sOff < 0 )
        strcpy( pOut, "");
    else {
        usRC = DecompressString( (PSZ) pIn+sOff, pOut );
        if ( usRC ) {
            pszTmp = (PSZ) calloc( 4 * usRC + 1, sizeof( char ));
            if ( pszTmp ) {
                USHORT i, j;

                strcpy( pszTmp, (PSZ) pOut );
                usRC = strlen( pszTmp );
                for ( i = 0, j = 0; i < usRC; i++ ) {
                    if ( pszTmp[ i ] < 32 || pszTmp[ i ] > 127 ) {
                        sprintf( (PSZ) pOut+j, "<%02X>", pszTmp[ i ] );
                        j+= 4;
                    }
                    else pOut[ j++ ] = pszTmp[ i ];
                }
                pOut[ j ] = 0;
                free( pszTmp );
            }
        }
        else if ( *((PSZ) pIn + sOff ))
            strcpy( pOut, (PSZ) pIn + sOff );
        else
            strcpy( pOut, "");
    }
    return ( pOut );
}


/* ------------------------------------------------------------------------- */
void ShowReadableData( PAK_DEV_DIRENTRY pdd, PBYTE pBuf )
{
    DESPPD     desPPD = {0};
    PBYTE      pInfoSeg,
               pScratch;
    PUI_BLOCK  puib;
    PUIC_BLOCK puicb;
    ULONG      ulCB;
    USHORT     i, j;
    PSHORT     psVal;
    PLONG      plVal;
    SHORT      sIdx, sPDX, sPDY;
    PSZ        psz;


    ulCB = sizeof( DESPPD );
    memcpy( (PBYTE) &desPPD, pBuf, ulCB );
    pInfoSeg = pBuf + ulCB;

    // Allocate and copy the dynamic data items in DESPPD
    ulCB = desPPD.stUIList.usBlockListSize;
    desPPD.stUIList.pBlockList = (PUI_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUIList.pBlockList, (PUI_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;

    ulCB = desPPD.stUICList.usNumOfUICs * sizeof( UIC_BLOCK );
    desPPD.stUICList.puicBlockList = (PUIC_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUICList.puicBlockList, (PUIC_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;
    desPPD.pPSStringBuff = pInfoSeg;

    // Create a scratch buffer for decompressing strings
    pScratch = (PBYTE) calloc( desPPD.desItems.iSizeBuffer, 1 );

    // Print header
    printf("==============================================================================\n");
    printf("PRINTER PAK ENTRY  -  \"%s\"\n", pdd.szDeviceName );
    printf("==============================================================================\n");

    printf("\nBasic Data\n----------\n");
    printf("Language level:                      %d\n", desPPD.desItems.usLanguageLevel );
    printf("Password:                            %s\n", OFFSET_TO_PSZ( desPPD.desItems.ofsPswrd, pInfoSeg ));
    printf("PPM:                                 %d\n", desPPD.desItems.iPpm );
    printf("FreeVM:                              %d\n", desPPD.desItems.lFreeVM );
    // AFAIK desPPD.desItems.ofsPrType is not used, but show it anyway
    printf("Printer type:                        %s\n", OFFSET_TO_PSZ( desPPD.desItems.ofsPrType, pInfoSeg ));
    printf("Printer name:                        %s\n", (PSZ)( pInfoSeg + desPPD.desItems.ofsPrName ));
    printf("ColorDevice:                         %d\n", desPPD.desItems.fIsColorDevice );
    printf("FileSystem:                          %d\n", desPPD.desItems.fIsFileSystem );
    printf("PC Filename:                         %s\n", (desPPD.desItems.ofsPCFileName >= 0) ?
                                                        (PSZ)( pInfoSeg + desPPD.desItems.ofsPCFileName ) :
                                                        "(none)");
    printf("Default DPI:                         %d\n", desPPD.desItems.iResDpi );
#if PSDRIVER == 1
    printf("TrueType font support:               %d\n", desPPD.desItems.fTTSupport );
#endif

    // I don't think the following are actually used; the available resolutions
    // are apparently defined only as UIOption items.
    if ( desPPD.desItems.ResList.uNumOfRes ) {
        if ( desPPD.desItems.ResList.bIsJCLResolution )
            printf("Defined JCL resolutions (%d)\n", desPPD.desItems.ResList.uNumOfRes );
        else
            printf("Defined resolutions (%d)\n", desPPD.desItems.ResList.uNumOfRes );
        if ( desPPD.desItems.ResList.uResOffset > 0 ) {
            psVal = (PSHORT)((PBYTE)(pInfoSeg + desPPD.desItems.ResList.uResOffset));
            for ( i = 0; i < desPPD.desItems.ResList.uNumOfRes; i++ ) {
                printf(" - %d\n", *psVal );
                psVal++;
            }
        }
    }

    printf("ScreenAngle:                         %d\n", desPPD.desItems.iScreenAngle );
    printf("ScreenFreq:                          %d\n", desPPD.desItems.lScrFreq );
    printf("Reset command:                       %s\n", OFFSET_TO_PSZ( desPPD.desItems.ofsReset, pInfoSeg ));
    printf("ExitServer command:                  %s\n", OffsetToCommand( desPPD.desItems.ofsExitserver, pInfoSeg, pScratch ));
    printf("Transfer Normalized command:         %s\n", OffsetToCommand( desPPD.desItems.ofsTransferNor, pInfoSeg, pScratch ));
    printf("Transfer Normalized.Inverse command: %s\n", OffsetToCommand( desPPD.desItems.ofsTransferInv, pInfoSeg, pScratch ));
    psz = OffsetToProperCommand( desPPD.desItems.ofsInitString, pInfoSeg, pScratch );
    printf("JCLBegin/InitPostScriptMode command: %s\n", strlen(psz)? psz: "(none)");
    psz = OffsetToProperCommand( desPPD.desItems.ofsJCLToPS, pInfoSeg, pScratch );
    printf("JCLToPSInterpreter command:          %s\n", strlen(psz)? psz: "(none)");
    psz = OffsetToProperCommand( desPPD.desItems.ofsTermString, pInfoSeg, pScratch );
    printf("JCLEnd/TermPostScriptMode command:   %s\n", strlen(psz)? psz: "(none)");

    printf("\nPage Properties\n---------------\n");
    // Several of these appear to be deprecated in favour of UI items and are
    // unused:
    //   desPage.iCmpgpairs
    //   desPage.ofsLspgCmnds
    //   printf("Default page size:                   %s\n", OFFSET_TO_PSZ( desPPD.desPage.ofsDfpgsz, pInfoSeg ));
    //   printf("Default imageable area:              %s\n", OFFSET_TO_PSZ( desPPD.desPage.ofsDefimagearea, pInfoSeg ));
    //   printf("Default paper dimensions:            %s\n", OFFSET_TO_PSZ( desPPD.desPage.ofsDefpaperdim, pInfoSeg ));
    printf("Variable paper:                      %d\n", desPPD.desPage.fIsVariablePaper );
    printf("Paper dimension pairs:               %d\n", desPPD.desPage.iDmpgpairs );
    psVal = (PSHORT)( pInfoSeg + desPPD.desPage.ofsDimxyPgsz );
    for ( i = 0; i < desPPD.desPage.iDmpgpairs; i++ ) {
        sIdx = (SHORT) *psVal;
        psVal++;
        sPDX = (SHORT) *psVal;
        psVal++;
        sPDY = (SHORT) *psVal;
        psVal++;
        printf("  %2d - %4d %4d\n", sIdx, sPDX, sPDY );
    }
    printf("Imageable coordinate pairs: %d\n", desPPD.desPage.iImgpgpairs );
    psVal = (PSHORT)( pInfoSeg + desPPD.desPage.ofsImgblPgsz );
    for ( i = 0; i < desPPD.desPage.iImgpgpairs; i++ ) {
        sIdx = (SHORT) *psVal;
        psVal++;
        sPDX = (SHORT) *psVal;
        psVal++;
        sPDY = (SHORT) *psVal;
        psVal++;
        printf("  %2d - %4d %4d ", sIdx, sPDX, sPDY );
        sPDX = (SHORT) *psVal;
        psVal++;
        sPDY = (SHORT) *psVal;
        psVal++;
        psz = (PSZ) psVal;
        printf("%4d %4d  (%s)\n", sPDX, sPDY, psz );
        psVal = (PSHORT) ((PSZ)( psz + strlen( psz ) + 1 ));
    }
    printf("Custom Page Size command:            %s\n", OffsetToCommand( desPPD.desPage.ofsCustomPageSize, pInfoSeg, pScratch ));
    printf("Custom Page min width:               %d\n", desPPD.desPage.iCustomPageSizeMinWidth );
    printf("Custom Page max width:               %d\n", desPPD.desPage.iCustomPageSizeMaxWidth );
    printf("Custom Page min height:              %d\n", desPPD.desPage.iCustomPageSizeMinHeight );
    printf("Custom Page max height:              %d\n", desPPD.desPage.iCustomPageSizeMaxHeight );

    printf("\nInput Trays\n-----------\n");
    // None of these items actually seem to be used or set anywhere; they
    // are presumably deprecated, as input slots are defined as UI items
    // (under desPPD.stUIList) in practice.
    printf("Manual Feed:                         %d\n", desPPD.desInpbins.iManualfeed );
    printf("Manual Feed set command:             %s\n", OffsetToCommand( desPPD.desInpbins.ofsManualtrue, pInfoSeg, pScratch ));
    printf("Manual Feed unset disable:           %s\n", OffsetToCommand( desPPD.desInpbins.ofsManualfalse, pInfoSeg, pScratch ));
    printf("Default input tray:                  %s\n", OFFSET_TO_PSZ( desPPD.desInpbins.ofsDefinputslot, pInfoSeg ));
    printf("Input tray pairs:                    %d\n", desPPD.desInpbins.iInpbinpairs );
    printf("Input tray paper sizes:              %d\n", desPPD.desInpbins.iNumOfPageSizes );
    // No need to even try to handle desPPD.desInpbins.ofsCmInpbins or
    // desPPD.desInpbins.ofsPageSizes -- they are deprecated and no longer used

    printf("\nOutput Trays\n------------\n");
    printf("Default output order:                %s\n", ( desPPD.desOutbins.fIsDefoutorder? "Reverse": "Normal" ));
    printf("Normal Output command:               %s\n", OffsetToCommand( desPPD.desOutbins.ofsOrdernormal, pInfoSeg, pScratch ));
    printf("Reverse Output command:              %s\n", OffsetToCommand( desPPD.desOutbins.ofsOrderreverse, pInfoSeg, pScratch ));
    printf("Default output tray:                 %s\n", OFFSET_TO_PSZ( desPPD.desOutbins.ofsDefoutputbin, pInfoSeg )); // not used?
    printf("Output tray pairs:                   %d\n", desPPD.desOutbins.iOutbinpairs );
    // desPPD.desOutbins.ofsCmOutbins is not used or set anywhere, so ignore it

    printf("\nFonts\n-----\n");
    printf("Default font:                        %s\n", OFFSET_TO_PSZ( desPPD.desFonts.ofsDeffont, pInfoSeg ));
    printf("Supported hardware fonts:            %d\n", desPPD.desFonts.iFonts );
    psz = OFFSET_TO_PSZ( desPPD.desFonts.ofsFontnames, pInfoSeg );
    for ( i = 0; (i < desPPD.desFonts.iFonts) && *psz; i++ ) {
        printf("  - %s\n", psz );
        psz += strlen( psz ) + 1;
    }

    printf("\nForms\n-----\n");
    // Not entirely sure these are used at all either
    printf("Number of forms:                     %d\n", desPPD.desForms.usFormCount );
    if ( desPPD.desForms.usFormCount ) {
        plVal = (PLONG)(pInfoSeg + desPPD.desForms.ofsFormIndex);
        for ( i = 0; (i < desPPD.desForms.usFormCount) && *psz; i++ ) {
            printf("  - %s\n", OffsetToCommand( (SHORT) *plVal, pInfoSeg, pScratch ));
            plVal++;
        }
    }

    printf("\n\nUser Interface Items\n--------------------\n");
    printf("Total size of UI list:               %d\n", desPPD.stUIList.usBlockListSize );
    printf("Number of UI items:                  %d\n", desPPD.stUIList.usNumOfBlocks );
    puib = desPPD.stUIList.pBlockList;
    for ( i = 0; puib && desPPD.stUIList.usBlockListSize && i < desPPD.stUIList.usNumOfBlocks; i++ ) {
        printf("\n* \"%s\"  (%d)\n", OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg ), i );
        printf("   Translation string:               \"%s\"\n", OFFSET_TO_PSZ( puib->ofsUITransString, pInfoSeg ));
        printf("   Order index value:                %d\n", puib->usOrderDep );
        printf("   Display order:                    %d\n", puib->usDisplayOrder );
        printf("   Location:                         ");
        switch( puib->usUILocation ) {
            case UI_ORDER_ANYSETUP   : printf("Any\n");        break;
            case UI_ORDER_JCLSETUP   : printf("JCL\n");        break;
            case UI_ORDER_PAGESETUP  : printf("Page\n");       break;
            case UI_ORDER_DOCSETUP   : printf("Document\n");   break;
            case UI_ORDER_PROLOGSETUP: printf("Prolog\n");     break;
            case UI_ORDER_EXITSERVER : printf("ExitServer\n"); break;
            default                  : printf("Unknown\n");    break;
        }
        printf("   UI selection type:                ");
        switch( puib->usSelectType ) {
            case UI_SELECT_BOOLEAN : printf("Boolean\n");  break;
            case UI_SELECT_PICKMANY: printf("PickMany\n"); break;
            case UI_SELECT_PICKONE : printf("PickOne\n");  break;
            default                : printf("Unknown\n");  break;
        }
        printf("   Scope:                            %s\n",
                ( puib->ucGroupType == UIGT_INSTALLABLEOPTION ) ? "Printer property": "Job property");
        printf("   Panel ID:                         ");
        switch ( puib->ucPanelID ) {
            case UIP_OS2_FEATURE   : printf("IBM\n");  break;
            case UIP_OEM_FEATURE   : printf("OEM\n");  break;
            case UIP_PREDEF_FEATURE: printf("Predefined\n");  break;
            default                : printf("Unknown\n");  break;
        }
        printf("   Default value:                    %d\n", puib->usDefaultEntry );
        printf("   Number of values:                 %d\n", puib->usNumOfEntries );
        if ( puib->usNumOfEntries ) {
            for ( j = 0; j < puib->usNumOfEntries; j++ ) {
                printf("   - Name:                           \"%s\"  (%d)\n", OFFSET_TO_PSZ( puib->uiEntry[j].ofsOption, pInfoSeg ), j );
                printf("     Translation:                    \"%s\"\n", OFFSET_TO_PSZ( puib->uiEntry[j].ofsTransString, pInfoSeg ));
                printf("     Value:                          %s\n", OffsetToCommand( puib->uiEntry[j].ofsValue, pInfoSeg, pScratch ));
            }
        }
        INCREMENT_BLOCK_PTR( puib );
    }

    printf("\n\nUser Interface Constraints\n--------------------------\n");
    printf("Number of mutually exlusive item sets: %d\n", desPPD.stUICList.usNumOfUICs );
    puicb = desPPD.stUICList.puicBlockList;
    for ( i = 0; puicb && i < desPPD.stUICList.usNumOfUICs; i++ ) {

        /*
        ** Despite the name, the "ofsUIBlock" values are not pointer offsets;
        ** rather, they are the index of the corresponding UI_LIST item in
        ** "stUIList.pBlockList".  "bOption" is a bitmap flagging the affected
        ** values (that is, items in the item's uiEntry[] array).  e.g.
        **      ofsUIBlock = 2
        **      bOption    = 13  (binary 1101)
        ** indicates the 1st, 3rd, and 4th values of the third UI item.
        */
        puib = desPPD.stUIList.pBlockList;
        for ( j = 0; j < puicb->uicEntry1.ofsUIBlock; j++ )
            INCREMENT_BLOCK_PTR( puib );
        printf(" - (%s", OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg ));
        if ( puicb->uicEntry1.bOption ) {
            printf(" ==");
            for ( j = 0; j < 32 && j < puib->usNumOfEntries; j++ ) {
                if (( puicb->uicEntry1.bOption >> j ) & 1 )
                    printf(" %s", OFFSET_TO_PSZ( puib->uiEntry[j].ofsOption, pInfoSeg ));
            }
        }
        printf(") with ");

        puib = desPPD.stUIList.pBlockList;
        for ( j = 0; j < puicb->uicEntry2.ofsUIBlock; j++ )
            INCREMENT_BLOCK_PTR( puib );
        printf("(%s", OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg ));
        if ( puicb->uicEntry2.bOption ) {
            printf(" ==");
            for ( j = 0; j < 32 && j < puib->usNumOfEntries; j++ ) {
                if (( puicb->uicEntry2.bOption >> j ) & 1 )
                    printf(" %s", OFFSET_TO_PSZ( puib->uiEntry[j].ofsOption, pInfoSeg ));
            }
        }
        printf(")\n");

        puicb++;
    }

    free( desPPD.stUIList.pBlockList );
    free( desPPD.stUICList.puicBlockList );
    free( pScratch );
}


/* ------------------------------------------------------------------------- *
 * PrintToPPD                                                                *
 *                                                                           *
 * Print a parameter/string value pair, formatted for PPD output.            *
 *                                                                           *
 * PARAMETERS:                                                               *
 *   PSZ pszName   : Name of the parameter as it will appear in the PPD      *
 *                   (must start with * and include a trailing colon)        *
 *   SHORT usOffset: Offset of the string value within the buffer            *
 *   PBYTE pBuf    : Data buffer from the PAK file                           *
 *   PSZ pszDefault: Default value in case there is no value defined in the  *
 *                   buffer; specify NULL to omit the parameter entirely in  *
 *                   such a case                                             *
 *                                                                           *
 * RETURNS: N/A                                                              *
 * ------------------------------------------------------------------------- */
void PrintToPPD( PSZ pszName, SHORT usOffset, PBYTE pBuf, PSZ pszDefault )
{
    if ( usOffset < 1 ) {
        if ( pszDefault ) printf("%-23s \"%s\"\n", pszName, pszDefault );
    }
    else
        printf("%-23s \"%s\"\n", pszName, OFFSET_TO_PSZ( usOffset, pBuf ));

    return;
}


/* ------------------------------------------------------------------------- */
void GeneratePPD( PBYTE pBuf )
{
    DESPPD     desPPD = {0};        // structure of main descriptor segment
    PBYTE      pInfoSeg,            // pointer to free-form information segment
               pScratch;            // work buffer, mostly for decompressing commands
    PUI_BLOCK  puib,                // pointer to a UI block
               puiPaper;            // pointer to the PageSize UI block
    PUIC_BLOCK puicb;               // pointer to a UI constraints block
    ULONG      ulCB;
    USHORT     usRC,
               i, j, k;
    PSHORT     psVal;
    PLONG      plVal;
    SHORT      sIdx,                // form index cross-reference
               sX1, sY1, sX2, sY2;  // area coordinates
    PSZ        psz,                 // general-purpose string pointer
               pszName,             // current UI item or form name
               pszXlate,            // current UI item or form translation name
               pszDefault,          // current UI item default
               pszDefPage;          // name of default PageSize


    // Copy the buffer contents into our data structure
    ulCB = sizeof( DESPPD );
    memcpy( (PBYTE) &desPPD, pBuf, ulCB );
    pInfoSeg = pBuf + ulCB;

    // Allocate and copy the dynamic data items in DESPPD
    ulCB = desPPD.stUIList.usBlockListSize;
    desPPD.stUIList.pBlockList = (PUI_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUIList.pBlockList, (PUI_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;

    ulCB = desPPD.stUICList.usNumOfUICs * sizeof( UIC_BLOCK );
    desPPD.stUICList.puicBlockList = (PUIC_BLOCK) malloc( ulCB );
    memcpy( desPPD.stUICList.puicBlockList, (PUIC_BLOCK) pInfoSeg, ulCB );
    pInfoSeg += ulCB;
    desPPD.pPSStringBuff = pInfoSeg;

    // Create a scratch buffer for decompressing strings
    pScratch = (PBYTE) calloc( desPPD.desItems.iSizeBuffer, 1 );

    //
    // Required headers
    //
    printf("*PPD-Adobe:             \"4.3\"\n");
    printf("*FormatVersion:         \"4.3\"\n");
    printf("*FileVersion:           \"1.0\"\n");
    printf("*LanguageVersion:       English\n");
    printf("*LanguageEncoding:      OS2-850\n");
    printf("*Manufacturer:          \"Autogenerated by pakfile utility\"\n");

    //
    // Identification & version parameters
    //
    printf("*Product:               \"(%s)\"\n", (PSZ)( pInfoSeg + desPPD.desItems.ofsPrName ));
    printf("*ModelName:             \"%s\"\n",   (PSZ)( pInfoSeg + desPPD.desItems.ofsPrName ));
    printf("*ShortNickName:         \"%s\"\n",   (PSZ)( pInfoSeg + desPPD.desItems.ofsPrName ));
    printf("*NickName:              \"%s\"\n",   (PSZ)( pInfoSeg + desPPD.desItems.ofsPrName ));
    printf("*PCFileName:            \"%s\"\n", (desPPD.desItems.ofsPCFileName >= 0) ?
                                               (PSZ)( pInfoSeg + desPPD.desItems.ofsPCFileName ) :
                                               "PRINTER.PPD");
    printf("*PSVersion:             \"(%d) 001\"\n", ((desPPD.desItems.usLanguageLevel < 2) ? 0 :
                                                      (desPPD.desItems.usLanguageLevel * 1000)) + 10 );
    printf("*Languagelevel:         \"%d\"\n", desPPD.desItems.usLanguageLevel );

    //
    // Basic capabilities
    //
    printf("*ColorDevice:           %s\n", (desPPD.desItems.fIsColorDevice == 1) ? "True": "False");
    printf("*FileSystem:            %s\n", (desPPD.desItems.fIsFileSystem == 1)  ? "True": "False");
#if PSDRIVER == 1
    if ( desPPD.desItems.fTTSupport == 1 )
        printf("*TTRasterizer:          Type42\n");
#endif
    if ( desPPD.desItems.iPpm > 0 )
        printf("*Throughput:            \"%d\"\n", desPPD.desItems.iPpm );
    if ( desPPD.desItems.lFreeVM > 0 )
        printf("*FreeVM:                \"%d\"\n", desPPD.desItems.lFreeVM );

    PrintToPPD("*Password:", desPPD.desItems.ofsPswrd, pInfoSeg, NULL );
    if (( desPPD.desItems.ofsReset > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desItems.ofsReset, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*Reset:                 \"%s\"\n", pScratch );
    }
    if (( desPPD.desItems.ofsExitserver > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desItems.ofsExitserver, pInfoSeg), pScratch ) > 0 ))
    {
        printf("*ExitServer:            \"%s\"\n", pScratch );
    }
    if ( desPPD.desItems.ofsInitString >= 0 )
        printf("*JCLBegin:              \"%s\"\n",
                OffsetToProperCommand( desPPD.desItems.ofsInitString, pInfoSeg, pScratch ));
    if ( desPPD.desItems.ofsJCLToPS >= 0 )
        printf("*JCLToPSInterpreter:    \"%s\"\n",
                OffsetToProperCommand( desPPD.desItems.ofsJCLToPS, pInfoSeg, pScratch ));
    if ( desPPD.desItems.ofsTermString >= 0 )
        printf("*JCLEnd:                \"%s\"\n",
                OffsetToProperCommand( desPPD.desItems.ofsTermString, pInfoSeg, pScratch ));

    //
    // Halftone options
    //
    if ( desPPD.desItems.iScreenAngle > 0 )
        printf("*ScreenAngle:           \"%.2f\"\n", desPPD.desItems.iScreenAngle / 100.0 );
    if ( desPPD.desItems.lScrFreq > 0 )
        printf("*ScreenFreq:            \"%.2f\"\n", desPPD.desItems.lScrFreq / 100.0 );
    if (( desPPD.desItems.ofsTransferNor > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desItems.ofsTransferNor, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*Transfer Normalized:   \"%s\"\n", pScratch );
    }
    if (( desPPD.desItems.ofsTransferInv > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desItems.ofsTransferInv, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*Transfer Normalized.Inverse: \"%s\"\n", pScratch );
    }
    printf("\n");

    //
    // Page & media handling
    //

    /*
    ** Find the PageSize UI block (every valid PPD should have one) and save a
    ** pointer to it.  We'll need it at various points from here on down.
    */
    puiPaper = NULL;
    puib = desPPD.stUIList.pBlockList;
    for ( i = 0; !puiPaper && i < desPPD.stUIList.usBlockListSize; i++ ) {
        psz = OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg );
        if ( !strcmp( psz, "PageSize"))
            puiPaper = puib;
        INCREMENT_BLOCK_PTR( puib );
    }
    // Make note of the default value; this indicates the default paper size
    if ( puiPaper && ( puiPaper->usNumOfEntries > puiPaper->usDefaultEntry ))
        pszDefPage = OFFSET_TO_PSZ( puiPaper->uiEntry[puiPaper->usDefaultEntry].ofsOption, pInfoSeg );
    else
        pszDefPage = "Letter";

    printf("*VariablePaperSize:     %s\n", (desPPD.desPage.fIsVariablePaper == 1) ? "True" : "False");

    /*
    ** The paper commands from desPage, plus everything in desInpbins, are
    ** basically deprecated; these are all defined as UI items these days.
    ** So we don't need to handle them (PIN doesn't write any of it into the
    ** PAK file anyway).
    ** The same goes for almost everything in desOutbins, but the following
    ** do appear to be used to some extent.
    */
    printf("*DefaultOutputOrder:    %s\n", (desPPD.desOutbins.fIsDefoutorder == REVERSE)? "Reverse": "Normal");
    if (( desPPD.desOutbins.ofsOrdernormal > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desOutbins.ofsOrdernormal, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*OutputOrder Normal:  \"%s\"\n", pScratch );
    }
    if (( desPPD.desOutbins.ofsOrderreverse > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desOutbins.ofsOrderreverse, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*OutputOrder Reverse: \"%s\"\n", pScratch );
    }
    // It's somewhat less clear, but desForms also seems to be unused nowadays.
    printf("\n");

    // desPage.ofsDefimagearea is unused; use pszDefPage instead
    printf("*DefaultImageableArea: %s\n", pszDefPage );
    psVal = (PSHORT)( pInfoSeg + desPPD.desPage.ofsImgblPgsz );
    for ( i = 0; i < desPPD.desPage.iImgpgpairs; i++ ) {
        pszName = NULL;
        sIdx = (SHORT) *psVal;
        psVal++;
        sX1 = (SHORT) *psVal;       // lower left X
        psVal++;
        sY1 = (SHORT) *psVal;       // lower left Y
        psVal++;
        sX2 = (SHORT) *psVal;       // upper right X
        psVal++;
        sY2 = (SHORT) *psVal;       // upper right Y
        psVal++;
        pszXlate = (PSZ) psVal;     // translation string (if any)

        // Get the actual form name from the *PageSize UI list
        if ( puiPaper && ( puiPaper->usNumOfEntries > puiPaper->usDefaultEntry )) {
            pszName = OFFSET_TO_PSZ( puiPaper->uiEntry[sIdx].ofsOption, pInfoSeg );
            printf("*ImageableArea %s/%s: \"%d %d %d %d\"\n", pszName,
                    (*pszXlate? pszXlate: pszName), sX1, sY1, sX2, sY2 );
        }
        psVal = (PSHORT) ((PSZ)( pszXlate + strlen( pszXlate ) + 1 ));
    }
    printf("\n");

    // desPage.ofsDefpaperdim is also unused; again, use pszDefPage
    printf("*DefaultPaperDimension: %s\n", pszDefPage );
    psVal = (PSHORT)( pInfoSeg + desPPD.desPage.ofsDimxyPgsz );
    for ( i = 0; i < desPPD.desPage.iDmpgpairs; i++ ) {
        pszName = NULL;
        sIdx = (SHORT) *psVal;
        psVal++;
        sX1 = (SHORT) *psVal;
        psVal++;
        sY1 = (SHORT) *psVal;
        psVal++;

        // Get the form name from the *PageSize UI list
        if ( puiPaper && ( puiPaper->usNumOfEntries > puiPaper->usDefaultEntry )) {
            pszName = OFFSET_TO_PSZ( puiPaper->uiEntry[sIdx].ofsOption, pInfoSeg );
            pszXlate = ( puiPaper->uiEntry[sIdx].ofsTransString > 0 ) ?
                         OFFSET_TO_PSZ( puiPaper->uiEntry[sIdx].ofsTransString, pInfoSeg ) :
                         pszName;
            printf("*PaperDimension %s/%s: \"%d %d\"\n", pszName, pszXlate, sX1, sY1 );
        }
    }
    printf("\n");

    if (( desPPD.desPage.ofsCustomPageSize > 0 ) &&
        ( DecompressString( OFFSET_TO_PSZ( desPPD.desPage.ofsCustomPageSize, pInfoSeg ), pScratch ) > 0 ))
    {
        printf("*CustomPageSize True: \"%s\"\n", pScratch );
        /*
        ** We have to hardcode the order because the PAK file doesn't contain
        ** that information.  It's missing a couple of supposedly-required
        ** parameters as well, but there's nothing we can do about that either.
        ** Anyway, if they're not in the PAK file, the PS driver obviously
        ** doesn't need/use them in any case.
        */
        printf("*ParamCustomPageSize Width: 1 points %d %d\n",
                desPPD.desPage.iCustomPageSizeMinWidth,
                desPPD.desPage.iCustomPageSizeMaxWidth );
        printf("*ParamCustomPageSize Height: 2 points %d %d\n",
                desPPD.desPage.iCustomPageSizeMinHeight,
                desPPD.desPage.iCustomPageSizeMaxHeight );
        printf("\n");
    }

    //
    // Write out the UI constraints, if any
    //
    puicb = desPPD.stUICList.puicBlockList;
    for ( i = 0; puicb && i < desPPD.stUICList.usNumOfUICs; i++ ) {
        PSZ pszName1, pszName2,
            pszVal1, pszVal2;
        PUI_BLOCK puib2;

        puib = desPPD.stUIList.pBlockList;
        for ( j = 0; j < puicb->uicEntry1.ofsUIBlock; j++ )
            INCREMENT_BLOCK_PTR( puib );
        pszName1 = OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg );
        if ( puicb->uicEntry1.bOption ) {
            for ( j = 0; j < 32 && j < puib->usNumOfEntries; j++ ) {
                if (( puicb->uicEntry1.bOption >> j ) & 1 ) {
                    pszVal1 = OFFSET_TO_PSZ( puib->uiEntry[j].ofsOption, pInfoSeg );
                    puib2 = desPPD.stUIList.pBlockList;
                    for ( k = 0; k < puicb->uicEntry2.ofsUIBlock; k++ )
                        INCREMENT_BLOCK_PTR( puib2 );
                    pszName2 = OFFSET_TO_PSZ( puib2->ofsUIName, pInfoSeg );
                    if ( puicb->uicEntry2.bOption ) {
                        for ( k = 0; k < 32 && k < puib2->usNumOfEntries; k++ ) {
                            if (( puicb->uicEntry2.bOption >> k ) & 1 ) {
                                pszVal2 = OFFSET_TO_PSZ( puib2->uiEntry[k].ofsOption, pInfoSeg );
                                printf("*UIConstraints: *%s %s *%s %s\n",
                                        pszName1, pszVal1, pszName2, pszVal2 );
                            }
                        }
                    }
                }
            }
        }
        puicb++;
    }
    printf("\n");

    //
    // OK, now do the UI items
    //
    puib = desPPD.stUIList.pBlockList;
    for ( i = 0; puib && desPPD.stUIList.usBlockListSize && i < desPPD.stUIList.usNumOfBlocks; i++ ) {
        psz = OFFSET_TO_PSZ( puib->ofsUIName, pInfoSeg );

        printf("*OpenUI *%s/%s: ", psz,
                OFFSET_TO_PSZ( puib->ofsUITransString, pInfoSeg ));
        switch( puib->usSelectType ) {
            case UI_SELECT_BOOLEAN : printf("Boolean\n");  break;
            case UI_SELECT_PICKMANY: printf("PickMany\n"); break;
            case UI_SELECT_PICKONE :
            default                : printf("PickOne\n");  break;
        }

        // Write the order dependency line
        printf("*OrderDependency: %d ", puib->usOrderDep + 1 );
        switch( puib->usUILocation ) {
            default:
            case UI_ORDER_ANYSETUP   : printf("AnySetup ");      break;
            case UI_ORDER_JCLSETUP   : printf("JCLSetup ");      break;
            case UI_ORDER_PAGESETUP  : printf("PageSetup ");     break;
            case UI_ORDER_DOCSETUP   : printf("DocumentSetup "); break;
            case UI_ORDER_PROLOGSETUP: printf("Prolog ");        break;
            case UI_ORDER_EXITSERVER : printf("ExitServer ");    break;
        }
        printf("*%s\n", psz );

        if ( strcmp( psz, "PageSize") == 0 ) {
            // We already saved the default, no need to jump through hoops now
            pszDefault = pszDefPage;
        }
        else if ( strcmp( psz, "Resolution") == 0 ) {
            // Try a few different ways to determine the default resolution
            if ( puib->usNumOfEntries > puib->usDefaultEntry )
                pszDefault = OFFSET_TO_PSZ( puib->uiEntry[puib->usDefaultEntry].ofsOption, pInfoSeg );
            else if ( desPPD.desItems.iResDpi > 0 ) {
                sprintf( pScratch, "%ddpi", desPPD.desItems.iResDpi );
                pszDefault = pScratch;
            }
            else if ( puib->usNumOfEntries )
                pszDefault = OFFSET_TO_PSZ( puib->uiEntry[0].ofsOption, pInfoSeg );
            else
                pszDefault = "300dpi";
        }
        else {
            // Find the default; if there's no default, just use the first item
            if ( puib->usNumOfEntries > puib->usDefaultEntry )
                pszDefault = OFFSET_TO_PSZ( puib->uiEntry[puib->usDefaultEntry].ofsOption, pInfoSeg );
            else if ( puib->usNumOfEntries )
                pszDefault = OFFSET_TO_PSZ( puib->uiEntry[0].ofsOption, pInfoSeg );
            else
                pszDefault = "Unknown";     // hopefully shouldn't happen
        }
        printf("*Default%s: %s\n", psz, pszDefault );

        // Now write the list of actual values
        for ( j = 0; j < puib->usNumOfEntries; j++ ) {
            pszName = OFFSET_TO_PSZ( puib->uiEntry[j].ofsOption, pInfoSeg );
            pszXlate = ( puib->uiEntry[j].ofsTransString > 0 ) ?
                       OFFSET_TO_PSZ( puib->uiEntry[j].ofsTransString, pInfoSeg ) :
                       pszName;
            printf("*%s %s/%s: ", psz, pszName, pszXlate );
            if (( puib->uiEntry[j].ofsValue > 0 ) &&
                ( DecompressString( OFFSET_TO_PSZ( puib->uiEntry[j].ofsValue, pInfoSeg ), pScratch ) > 0 ))
            {
                printf("\"%s\"\n", pScratch );
            } else
                printf("\"\"\n");
        }

        // Do we need to do anything with this?
        // puib->ucGroupType

        printf("*CloseUI: *%s\n", psz );
        printf("\n");
        INCREMENT_BLOCK_PTR( puib );
    }

    //
    // Lastly, the supported hardware fonts
    //
    if ( desPPD.desFonts.ofsDeffont > 0 )
        printf("*DefaultFont: %s\n", OFFSET_TO_PSZ( desPPD.desFonts.ofsDeffont, pInfoSeg ));
    psz = OFFSET_TO_PSZ( desPPD.desFonts.ofsFontnames, pInfoSeg );
    for ( i = 0; (i < desPPD.desFonts.iFonts) && *psz; i++ ) {
        // Just use some common values for the encoding/version/status; PIN
        // doesn't use or care about them anyway.
        printf("*Font %s: Standard \"(001.006S)\" Standard ROM\n", psz );
        psz += strlen( psz ) + 1;
    }
    printf("\n");

    // And we're done!

    // Clean up
    free( desPPD.stUIList.pBlockList );
    free( desPPD.stUICList.puicBlockList );
    free( pScratch );
}


//
// THESE FUNCTIONS STOLEN FROM UTLCHNL.C (USED FOR DECOMPRESSING STRINGS):
//

//*****************************************************************************
//
// FUNCTION: CharToHex
//
// DESCRIPTION: Converts an character to its hex rep
//
// Returns -1 if fails
//
//*****************************************************************************

BYTE CharToHex( CHAR c )
{
  if ( c >= '0' && c <= '9' )
  {
    return c - '0';
  }
  else
  if ( c >= 'a' && c <= 'f' )
  {
    return c - 'a' + 10;
  }
  if ( c >= 'A' && c <= 'F' )
  {
    return c - 'A' + 10;
  }

  return -1;
}

//*****************************************************************************
//
// FUNCTION: ProcessHexString
//
// DESCRIPTION: Will take a WELL FORMED hex string that is one that starts
// with less than char, is all hex digits - no white space, and even amount
// ending with greater than char
// Note - since function will adjust the callers in and out buffers the addr
// of the callers buufers are passesd hence the PSZ * type
//
// Defect 203803 some strings are not up to standards eg HP 1200C where they are
// in form <a><b> ....
// So we have to take that in account (Real World - you know)
//
// RETURNS:
// Returns the number of hex bytes - esentially the number of ASCII chars/2 -
// 2 for the <>
//
//*****************************************************************************

SHORT ProcessHexString( PSZ *ppszBuffIn,  // Ptr 2 Ptr of input buffer
                        PSZ *ppszBuffOut) // Ptr 2 Ptr of output buffer
{
  PSZ pIn  = *ppszBuffIn;
  PSZ pOut = *ppszBuffOut;
  INT iCount = 0;

  // Hop over initial < of hex string
  *pIn++;

  while ( *pIn != '>' )
  {
    *pOut = CharToHex( *pIn );
    pIn++;
    //-------------------- Georgs P.         203803
    if ( *pIn != '>' )
    {
      *pOut = (*pOut << 4) | CharToHex( *pIn );
      pIn++;
    }
    pOut++;
    iCount++;
  }

  // Adjust pointers
  *ppszBuffIn = pIn;
  *ppszBuffOut = pOut;

  return iCount;
}


/*****************************************************************************\
**
** FUNCTION NAME = DecompressString
**
** DESCRIPTION   = Decompress a string from ppd files that was
**                 compressed by the PPD compiler.
**
**
** INPUT         = pszBuffIn                - pointer to input buffer
**                 pszBuffOut               - pointer to output buffer
**
** OUTPUT        = returns length of output buffer
**
** RETURN-NORMAL = NONE
** RETURN-ERROR  = NONE
**
\*****************************************************************************/

USHORT DecompressString(PSZ pszBuffIn, PSZ pszBuffOut)
{
  SHORT  sAdjust;
  USHORT usIndex;
  SHORT  usOutSize;                     /* size of the output buffer    */
  USHORT usStringLen;
  USHORT usOffSet;

  usOutSize = 0;

  /*
  ** For each char in source
  */
  while ( *pszBuffIn )
  {

    // Ahh - must be either a hex string or dict entry
    if ( *pszBuffIn == '<' )
    {
      // Another < - must be dict entry
      // Add them to output line
      if ( *(pszBuffIn+1) == '<' )
      {
        *pszBuffOut++ = *pszBuffIn++;
        *pszBuffOut++ = *pszBuffIn;
        usOutSize += 2;
      }
      else
      {
        /*        ** "cng55p2.ppd" [UIsection=Halftone]; "601ps95.ppd [UIsection=Collate]"
        */
        if ( *(pszBuffIn+1) == ' '  || *(pszBuffIn+1) == '\n' ||
             *(pszBuffIn+1) == '\r' || *(pszBuffIn+1) == '\t' )
        {
           *pszBuffOut++ = *pszBuffIn;
           usOutSize++;
        }
        else
        {
          usOutSize += ProcessHexString( &pszBuffIn, &pszBuffOut );
        }
      }
    }
    else
    if (*pszBuffIn < 128)  //If regular character copy it over
    {
      *pszBuffOut = *pszBuffIn;
      usOutSize++;
      pszBuffOut++;
    }
    else          //Its a compressed char
    {
      sAdjust = -128;
      while (*pszBuffIn == 255) //Pass over flag bytes
      {
        pszBuffIn++;
        sAdjust += 254;         //This is the offset adjust
      }
      usIndex = (USHORT)*pszBuffIn + sAdjust; //This is the postion of offset
      usOffSet = sPSKeyWordOffset[usIndex];   //Actual offset into words buffer
      strcpy(pszBuffOut,&(achPSKeyWords[usOffSet]));  //Copy word to target
      usStringLen = strlen (&(achPSKeyWords[usOffSet]));
      usOutSize += usStringLen;  //Adjust pointers
      pszBuffOut += usStringLen;
    }
    pszBuffIn++;
  }
  *pszBuffOut = '\0';   //End with null byte
  return(usOutSize);
}



