/******************************Module*Header*******************************\
*
*                           *******************
*                           * GDI SAMPLE CODE *
*                           *******************
*
* Module Name: debug.c
*
* debug helpers routine
*
* Copyright (c) 1992-1998 Microsoft Corporation
*
\**************************************************************************/

#include "driver.h"

#if DBG

ULONG DebugLevel = 90;

/*****************************************************************************
 *
 *   Routine Description:
 *
 *      This function is variable-argument, level-sensitive debug print
 *      routine.
 *      If the specified debug level for the print statement is lower or equal
 *      to the current debug level, the message will be printed.
 *
 *   Arguments:
 *
 *      DebugPrintLevel - Specifies at which debugging level the string should
 *          be printed
 *
 *      DebugMessage - Variable argument ascii c string
 *
 *   Return Value:
 *
 *      None.
 *
 ***************************************************************************/

VOID DebugPrint(
    __in ULONG DebugPrintLevel,
    __in CHAR *DebugMessage,
    ...
    )
{
    va_list ap;

    UNREFERENCED_PARAMETER(DebugPrintLevel);

    va_start(ap, DebugMessage);

    if (DebugPrintLevel <= DebugLevel)
    {
        EngDebugPrint("", DebugMessage, ap);
    }

    va_end(ap);
}

#endif
