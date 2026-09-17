#ifndef BOF_API_H
#define BOF_API_H

#include <windows.h>

#define OUTPUT_TEXT  0x00
#define OUTPUT_ERROR 0x0d

DECLSPEC_IMPORT void BeaconPrintf(int type, const char *format, ...);

#endif
