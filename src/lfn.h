/********************************************************************
   lfn.h
   Copyright (c) Microsoft Corporation. All rights reserved.
   Licensed under the MIT License.
********************************************************************/

#include <windows.h>

#define MAXPATHLEN 1024
#define COUNTOF(x) (sizeof(x)/sizeof(*x))

DWORD WFJunction(LPCWSTR LinkDirectory, LPCWSTR LinkTarget);
DWORD DecodeReparsePoint(LPCWSTR szMyFile, LPWSTR szDest, DWORD cwcDest);