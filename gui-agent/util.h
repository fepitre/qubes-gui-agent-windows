/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once
#include <windows.h>

extern DWORD g_DisableCursor;

ULONG HideCursors(void);
ULONG DisableEffects(void);
HANDLE CreateNamedEvent(IN const WCHAR *name); // returns NULL on failure
HANDLE CreateNamedMailslot(IN const WCHAR *name); // returns NULL on failure
ULONG StartProcess(IN WCHAR *executable, OUT HANDLE *processHandle);
ULONG IncreaseProcessWorkingSetSize(IN SIZE_T minimumSize, IN SIZE_T maximumSize);
ULONG AttachToInputDesktop(void);
void PageToRect(ULONG pageNumber, OUT RECT *rect);
