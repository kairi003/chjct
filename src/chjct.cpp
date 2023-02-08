/********************************************************************
   chjct.cpp
   Tiny tool to overwrite junction links without removing.
   It is based in part on https://github.com/microsoft/winfile.
   Copyright (c) kairi. All rights reserved.
   Licensed under the MIT License.
********************************************************************/

#include <stdio.h>
#include <locale.h>
#include "lfn.h"

int wmain(int argc, wchar_t* argv[])
{
    setlocale(LC_ALL, "");

    if (argc != 3) {
        wprintf(L"chjct <JUNCTION_PATH> <NEW_TARGET_PATH>\n");
        return 1;
    }
    DWORD errorLevel = WFJunction((LPCWSTR)argv[1], (LPCWSTR)argv[2]);
    if (errorLevel) {
        fwprintf(stderr, L"Error: %ul\n", errorLevel);
    }
    return errorLevel;
}
