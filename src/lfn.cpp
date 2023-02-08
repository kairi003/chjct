/********************************************************************
   lfn.c
   This file contains code that combines winnet long filename API's and
   the DOS INT 21h API's into a single interface.  Thus, other parts of
   Winfile call a single piece of code with no worries about the
   underlying interface.
   Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License.
********************************************************************/

#include <stdio.h>
#include "lfn.h"

#define PATH_PARSE_SWITCHOFF L"\\\\?\\" 
#define PATH_PARSE_SWITCHOFF_SIZE (sizeof(PATH_PARSE_SWITCHOFF) - 1) / sizeof(wchar_t)
#define REPARSE_MOUNTPOINT_HEADER_SIZE   8

typedef struct _REPARSE_DATA_BUFFER {
    ULONG  ReparseTag;
    USHORT  ReparseDataLength;
    USHORT  Reserved;
    union {
        struct {
            USHORT  SubstituteNameOffset;
            USHORT  SubstituteNameLength;
            USHORT  PrintNameOffset;
            USHORT  PrintNameLength;
            ULONG   Flags; // it seems that the docu is missing this entry (at least 2008-03-07)
            WCHAR  PathBuffer[1];
        } SymbolicLinkReparseBuffer;
        struct {
            USHORT  SubstituteNameOffset;
            USHORT  SubstituteNameLength;
            USHORT  PrintNameOffset;
            USHORT  PrintNameLength;
            WCHAR  PathBuffer[1];
        } MountPointReparseBuffer;
        struct {
            UCHAR  DataBuffer[1];
        } GenericReparseBuffer;
    };
} REPARSE_DATA_BUFFER, * PREPARSE_DATA_BUFFER;

BOOL IsVeryLongPath(LPCWSTR pszPathName)
{
    return (wcslen(pszPathName) >= COUNTOF(PATH_PARSE_SWITCHOFF) - 1) && !wcsncmp(pszPathName, PATH_PARSE_SWITCHOFF, COUNTOF(PATH_PARSE_SWITCHOFF) - 1);
}

/* WFJunction
 *
 * Creates a NTFS Junction
 * Returns either ERROR_SUCCESS or GetLastError()
 */
DWORD WFJunction(LPCWSTR pszLinkDirectory, LPCWSTR pszLinkTarget)
{
    DWORD		dwRet = ERROR_SUCCESS;
    // Size assumption: We have to copy 2 path with each MAXPATHLEN long onto the structure. So we take 3 times MAXPATHLEN
    char		reparseBuffer[MAXPATHLEN * 3];
    WCHAR		szDirectoryName[MAXPATHLEN];
    WCHAR		szTargetName[MAXPATHLEN];
    PWCHAR	szFilePart;
    DWORD		dwLength;


    // Get the full path referenced by the target
    if (!GetFullPathName(pszLinkTarget, MAXPATHLEN, szTargetName, &szFilePart))
        return GetLastError();

    // Get the full path referenced by the directory
    if (!GetFullPathName(pszLinkDirectory, MAXPATHLEN, szDirectoryName, &szFilePart))
        return GetLastError();

    // Create the link - ignore errors since it might already exist
    BOOL bDirCreated = CreateDirectory(pszLinkDirectory, NULL);
    if (!bDirCreated) {
        DWORD dwErr = GetLastError();
        if (ERROR_ALREADY_EXISTS != dwErr)
            return dwErr;
        else {
            // If a Junction already exists, we have to check if it points to the 
            // same location, and if yes then return ERROR_ALREADY_EXISTS
            wchar_t szDestination[MAXPATHLEN] = { 0 };
            DecodeReparsePoint(pszLinkDirectory, szDestination, COUNTOF(szDestination));

            if (!_wcsicmp(szDestination, pszLinkTarget)) {
                SetLastError(ERROR_ALREADY_EXISTS);
                return ERROR_ALREADY_EXISTS;
            }
        }
    }

    HANDLE hFile = CreateFile(
        pszLinkDirectory,
        GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
        NULL
    );

    if (INVALID_HANDLE_VALUE == hFile)
        return GetLastError();

    // Make the native target name
    WCHAR szSubstituteName[MAXPATHLEN];

    // The target might be
    if (IsVeryLongPath(szTargetName)) {
        // a very long path: \\?\x:\path\target
        swprintf_s(szSubstituteName, MAXPATHLEN, L"\\??\\%s", &szTargetName[PATH_PARSE_SWITCHOFF_SIZE]);
    }
    else {
        if (szTargetName[0] == L'\\' && szTargetName[1] == L'\\')
            // an UNC name: \\myShare\path\target
            swprintf_s(szSubstituteName, MAXPATHLEN, L"\\??\\UNC\\%s", &szTargetName[2]);
        else
            // a normal full path: x:\path\target
            swprintf_s(szSubstituteName, MAXPATHLEN, L"\\??\\%s", szTargetName);
    }

    // Delete the trailing slashes for non root path x:\path\foo\ -> x:\path\foo, but keep x:\
    // Furthermore keep \\?\Volume{GUID}\ for 'root' volume-names
    size_t lenSub = wcslen(szSubstituteName);
    if ((szSubstituteName[lenSub - 1] == L'\\') && (szSubstituteName[lenSub - 2] != L':') && (szSubstituteName[lenSub - 2] != L'}'))
        szSubstituteName[lenSub - 1] = 0;

    PREPARSE_DATA_BUFFER reparseJunctionInfo = (PREPARSE_DATA_BUFFER)reparseBuffer;
    memset(reparseJunctionInfo, 0, sizeof(REPARSE_DATA_BUFFER));
    reparseJunctionInfo->ReparseTag = IO_REPARSE_TAG_MOUNT_POINT;

    reparseJunctionInfo->MountPointReparseBuffer.SubstituteNameOffset = 0x00;
    reparseJunctionInfo->MountPointReparseBuffer.SubstituteNameLength = (USHORT)(wcslen(szSubstituteName) * sizeof(wchar_t));
    wcscpy_s(reparseJunctionInfo->MountPointReparseBuffer.PathBuffer, MAXPATHLEN, szSubstituteName);

    reparseJunctionInfo->MountPointReparseBuffer.PrintNameOffset = reparseJunctionInfo->MountPointReparseBuffer.SubstituteNameLength + sizeof(wchar_t);
    reparseJunctionInfo->MountPointReparseBuffer.PrintNameLength = (USHORT)(wcslen(szTargetName) * sizeof(wchar_t));
    wcscpy_s(reparseJunctionInfo->MountPointReparseBuffer.PathBuffer + wcslen(szSubstituteName) + 1, MAXPATHLEN, szTargetName);

    reparseJunctionInfo->ReparseDataLength = (USHORT)(reparseJunctionInfo->MountPointReparseBuffer.SubstituteNameLength +
        reparseJunctionInfo->MountPointReparseBuffer.PrintNameLength +
        FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer.PathBuffer[2]) - FIELD_OFFSET(REPARSE_DATA_BUFFER, MountPointReparseBuffer));

    // Set the link
    //
    if (!DeviceIoControl(hFile, FSCTL_SET_REPARSE_POINT,
        reparseJunctionInfo,
        reparseJunctionInfo->ReparseDataLength + REPARSE_MOUNTPOINT_HEADER_SIZE,
        NULL, 0, &dwLength, NULL)) {
        dwRet = GetLastError();
        CloseHandle(hFile);
        RemoveDirectory(pszLinkDirectory);
        return dwRet;
    }

    CloseHandle(hFile);
    return ERROR_SUCCESS;
}

DWORD DecodeReparsePoint(LPCWSTR szFullPath, LPWSTR szDest, DWORD cwcDest)
{
    HANDLE hFile;
    DWORD dwBufSize = MAXIMUM_REPARSE_DATA_BUFFER_SIZE;
    REPARSE_DATA_BUFFER* rdata;
    DWORD dwRPLen, cwcLink = 0;
    DWORD reparseTag;
    BOOL bRP;

    hFile = CreateFile(szFullPath, FILE_READ_EA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (hFile == INVALID_HANDLE_VALUE)
        return IO_REPARSE_TAG_RESERVED_ZERO;

    // Allocate the reparse data structure
    rdata = (REPARSE_DATA_BUFFER*)LocalAlloc(LMEM_FIXED, dwBufSize);

    // Query the reparse data
    bRP = DeviceIoControl(hFile, FSCTL_GET_REPARSE_POINT, NULL, 0, rdata, dwBufSize, &dwRPLen, NULL);

    CloseHandle(hFile);

    if (!bRP)
    {
        LocalFree(rdata);
        return IO_REPARSE_TAG_RESERVED_ZERO;
    }

    reparseTag = rdata->ReparseTag;

    if (IsReparseTagMicrosoft(rdata->ReparseTag) &&
        (rdata->ReparseTag == IO_REPARSE_TAG_MOUNT_POINT || rdata->ReparseTag == IO_REPARSE_TAG_SYMLINK)
        )
    {
        cwcLink = rdata->SymbolicLinkReparseBuffer.SubstituteNameLength / sizeof(WCHAR);
        // NOTE: cwcLink does not include any '\0' termination character
        if (cwcLink < cwcDest)
        {
            LPWSTR szT = &rdata->SymbolicLinkReparseBuffer.PathBuffer[rdata->SymbolicLinkReparseBuffer.SubstituteNameOffset / sizeof(WCHAR)];

            // Handle ?\ prefix
            if (szT[0] == '?' && szT[1] == '\\')
            {
                szT += 2;
                cwcLink -= 2;
            }
            else
                // Handle \??\ prefix
                if (szT[0] == '\\' && szT[1] == '?' && szT[2] == '?' && szT[3] == '\\')
                {
                    szT += 4;
                    cwcLink -= 4;
                }
            wcsncpy_s(szDest, MAXPATHLEN, szT, cwcLink);
            szDest[cwcLink] = 0;
        }
        else
        {
            lstrcpy(szDest, L"<symbol link reference too long>");
        }
    }

    LocalFree(rdata);
    return reparseTag;
}

