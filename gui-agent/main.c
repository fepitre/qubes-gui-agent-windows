#include <windows.h>
#include <winsock2.h>
#include <mmsystem.h>
#include <stdlib.h>

#include <xenstore.h>

#include "main.h"
#include "vchan.h"
#include "qvcontrol.h"
#include "resolution.h"
#include "send.h"
#include "vchan-handlers.h"
#include "util.h"

// windows-utils
#include "log.h"
#include "config.h"

#include <strsafe.h>

#define FULLSCREEN_ON_EVENT_NAME L"WGA_FULLSCREEN_ON"
#define FULLSCREEN_OFF_EVENT_NAME L"WGA_FULLSCREEN_OFF"

// If set, only invalidate parts of the screen that changed according to
// qvideo's dirty page scan of surface memory buffer.
BOOL g_UseDirtyBits;
DWORD g_MaxFps = 0; // max refresh event batches per second that are sent to guid (0 = disabled)

LONG g_ScreenHeight;
LONG g_ScreenWidth;

BOOL g_VchanClientConnected = FALSE;
BOOL g_SeamlessMode = TRUE;

// used to determine whether our window in fullscreen mode should be borderless
// (when resolution is smaller than host's)
LONG g_HostScreenWidth = 0;
LONG g_HostScreenHeight = 0;

char g_DomainName[256] = "<unknown>";

LIST_ENTRY g_WatchedWindowsList;
CRITICAL_SECTION g_csWatchedWindows;
BANNED_WINDOWS g_bannedWindows = { 0 };

HWND g_DesktopWindow = NULL;

HANDLE g_ShutdownEvent = NULL;

ULONG ProcessUpdatedWindows(IN HDC screenDC);

// wdk 7.1 doesn't have psapi.h
DWORD WINAPI GetProcessImageFileNameW(
    HANDLE Process,
    WCHAR *ImageFileName,
    DWORD Size
    );

#ifdef DEBUG
// diagnostic: dump all watched windows
void DumpWindows(void)
{
    WINDOW_DATA *entry;
    WCHAR exePath[MAX_PATH];
    DWORD pid = 0;
    HANDLE process;

    EnterCriticalSection(&g_csWatchedWindows);
    entry = (WINDOW_DATA *) g_WatchedWindowsList.Flink;

    while (entry != (WINDOW_DATA *) &g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);

        exePath[0] = 0;
        GetWindowThreadProcessId(entry->WindowHandle, &pid);
        if (pid)
        {
            process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (process)
            {
                GetProcessImageFileNameW(process, exePath, RTL_NUMBER_OF(exePath));
                CloseHandle(process);
            }
        }

        LogDebug("%8x: (%6d,%6d) %4dx%4d ico=%d ovr=%d [%s] '%s' {%s}",
            entry->WindowHandle, entry->X, entry->Y, entry->Width, entry->Height,
            entry->IsIconic, entry->IsOverrideRedirect,
            entry->Class, entry->Caption, exePath);

        entry = (WINDOW_DATA *) entry->ListEntry.Flink;
    }

    LeaveCriticalSection(&g_csWatchedWindows);
}
#endif

// watched windows list critical section must be entered
// Returns ERROR_SUCCESS if the window was added OR ignored (windowEntry is NULL if ignored).
// Other errors mean fatal conditions.
ULONG AddWindowWithInfo(IN HWND window, IN const WINDOWINFO *windowInfo, OUT WINDOW_DATA **windowEntry OPTIONAL)
{
    WINDOW_DATA *entry = NULL;
    ULONG status;

    if (!windowInfo)
        return ERROR_INVALID_PARAMETER;

    LogDebug("0x%x (%d,%d)-(%d,%d), style 0x%x, exstyle 0x%x, visible=%d",
        window, windowInfo->rcWindow.left, windowInfo->rcWindow.top, windowInfo->rcWindow.right, windowInfo->rcWindow.bottom,
        windowInfo->dwStyle, windowInfo->dwExStyle, IsWindowVisible(window));

    entry = FindWindowByHandle(window);
    if (entry) // already in list
    {
        if (windowEntry)
            *windowEntry = entry;
        return ERROR_SUCCESS;
    }

    entry = (WINDOW_DATA *) malloc(sizeof(WINDOW_DATA));
    if (!entry)
    {
        LogError("Failed to malloc entry");
        return ERROR_NOT_ENOUGH_MEMORY;
    }

    ZeroMemory(entry, sizeof(WINDOW_DATA));

    entry->WindowHandle = window;
    entry->X = windowInfo->rcWindow.left;
    entry->Y = windowInfo->rcWindow.top;
    entry->Width = windowInfo->rcWindow.right - windowInfo->rcWindow.left;
    entry->Height = windowInfo->rcWindow.bottom - windowInfo->rcWindow.top;
    entry->IsIconic = IsIconic(window);
    GetWindowText(window, entry->Caption, RTL_NUMBER_OF(entry->Caption)); // don't really care about errors here
    GetClassName(window, entry->Class, RTL_NUMBER_OF(entry->Class));

    LogDebug("0x%x: iconic=%d", entry->WindowHandle, entry->IsIconic);

    // FIXME: better prevention of large popup windows that can obscure dom0 screen
    // this is mainly for the logon window (which is screen-sized without caption)
    if (entry->Width == g_ScreenWidth && entry->Height == g_ScreenHeight)
    {
        LogDebug("popup too large: %dx%d, screen %dx%d",
            entry->Width, entry->Height, g_ScreenWidth, g_ScreenHeight);
        entry->IsOverrideRedirect = FALSE;
    }
    else
    {
        // WS_CAPTION is defined as WS_BORDER | WS_DLGFRAME, must check both bits
        if ((windowInfo->dwStyle & WS_CAPTION) == WS_CAPTION)
        {
            // normal window
            entry->IsOverrideRedirect = FALSE;
        }
        else if (((windowInfo->dwStyle & WS_SYSMENU) == WS_SYSMENU) && ((windowInfo->dwExStyle & WS_EX_APPWINDOW) == WS_EX_APPWINDOW))
        {
            // Metro apps without WS_CAPTION.
            // MSDN says that windows with WS_SYSMENU *should* have WS_CAPTION,
            // but I guess MS doesn't adhere to its own standards...
            entry->IsOverrideRedirect = FALSE;
        }
        else
            entry->IsOverrideRedirect = TRUE;
    }

    if (entry->IsOverrideRedirect)
    {
        LogDebug("popup: %dx%d, screen %dx%d",
            windowInfo->rcWindow.right - windowInfo->rcWindow.left,
            windowInfo->rcWindow.bottom - windowInfo->rcWindow.top,
            g_ScreenWidth, g_ScreenHeight);
    }

    InsertTailList(&g_WatchedWindowsList, &entry->ListEntry);

    // send window creation info to gui daemon
    if (g_VchanClientConnected)
    {
        status = SendWindowCreate(entry);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowCreate");

        // map (show) the window if it's visible and not minimized
        if (!entry->IsIconic)
        {
            status = SendWindowMap(entry);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowMap");
        }

        status = SendWindowName(window, entry->Caption);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowName");
    }

    if (windowEntry)
        *windowEntry = entry;
    return ERROR_SUCCESS;
}

// Remove window from the list and free memory.
// Watched windows list critical section must be entered.
ULONG RemoveWindow(IN OUT WINDOW_DATA *entry)
{
    ULONG status;

    if (!entry)
        return ERROR_INVALID_PARAMETER;

    LogDebug("0x%x", entry->WindowHandle);

    RemoveEntryList(&entry->ListEntry);

    if (g_VchanClientConnected)
    {
        status = SendWindowUnmap(entry->WindowHandle);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowUnmap");

        if (entry->WindowHandle) // never destroy screen "window"
        {
            status = SendWindowDestroy(entry->WindowHandle);
            if (ERROR_SUCCESS != status)
                return perror2(status, "SendWindowDestroy");
        }
    }

    free(entry);

    return ERROR_SUCCESS;
}

// EnumWindows callback for adding all top-level windows to the list.
static BOOL CALLBACK AddWindowsProc(IN HWND window, IN LPARAM lParam)
{
    WINDOWINFO wi = { 0 };
    ULONG status;

    LogVerbose("window %x", window);

    wi.cbSize = sizeof(wi);
    if (!GetWindowInfo(window, &wi))
    {
        perror("GetWindowInfo");
        LogWarning("Skipping window %x", window);
        return TRUE;
    }

    if (!ShouldAcceptWindow(window, &wi))
        return TRUE; // skip

    status = AddWindowWithInfo(window, &wi, NULL);
    if (ERROR_SUCCESS != status)
    {
        perror2(status, "AddWindowWithInfo");
        return FALSE; // stop enumeration, fatal error occurred (should probably exit process at this point)
    }

    return TRUE;
}

// Adds all top-level windows to the watched list.
// watched windows critical section must be entered
static ULONG AddAllWindows(void)
{
    LogVerbose("start");

    // Check for special windows that should be ignored/hidden in seamless mode.
    g_bannedWindows.Explorer = FindWindow(L"Progman", L"Program Manager");

    g_bannedWindows.Taskbar = FindWindow(L"Shell_TrayWnd", NULL);
    if (g_bannedWindows.Taskbar)
    {
        ShowWindow(g_bannedWindows.Taskbar, SW_HIDE);
    }

    g_bannedWindows.Start = FindWindowEx(g_DesktopWindow, NULL, L"Button", NULL);
    if (g_bannedWindows.Start)
    {
        ShowWindow(g_bannedWindows.Start, SW_HIDE);
    }

    LogDebug("desktop=0x%x, explorer=0x%x, taskbar=0x%x, start=0x%x",
        g_DesktopWindow, g_bannedWindows.Explorer, g_bannedWindows.Taskbar, g_bannedWindows.Start);

    // Enum top-level windows and add all that are not filtered.
    if (!EnumWindows(AddWindowsProc, 0))
        return perror("EnumWindows");

    return ERROR_SUCCESS;
}

// Reinitialize hooks/watched windows, called after a seamless/fullscreen switch or resolution change.
// NOTE: this function doesn't close/reopen qvideo's screen section
static ULONG ResetWatch(BOOL seamlessMode)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    ULONG status;

    LogVerbose("start");

    LogDebug("removing all windows");
    // clear the watched windows list
    EnterCriticalSection(&g_csWatchedWindows);

    entry = (WINDOW_DATA *) g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *) &g_WatchedWindowsList)
    {
        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA *) entry->ListEntry.Flink;

        status = RemoveWindow(entry);
        if (ERROR_SUCCESS != status)
        {
            LeaveCriticalSection(&g_csWatchedWindows);
            return perror2(status, "RemoveWindow");
        }

        entry = nextEntry;
    }

    LeaveCriticalSection(&g_csWatchedWindows);

    g_DesktopWindow = NULL;

    status = ERROR_SUCCESS;

    // WatchForEvents will map the whole screen as one window.
    if (seamlessMode)
    {
        // Add all eligible windows to watch list.
        // Since this is a switch from fullscreen, no windows were watched.
        EnterCriticalSection(&g_csWatchedWindows);
        status = AddAllWindows();
        LeaveCriticalSection(&g_csWatchedWindows);
    }
    else
    {
        if (g_bannedWindows.Taskbar)
        {
            ShowWindow(g_bannedWindows.Taskbar, SW_SHOW);
        }

        if (g_bannedWindows.Start)
        {
            ShowWindow(g_bannedWindows.Start, SW_SHOW);
        }
    }

    LogVerbose("success");
    return status;
}

// set fullscreen/seamless mode
ULONG SetSeamlessMode(IN BOOL seamlessMode, IN BOOL forceUpdate)
{
    ULONG status;

    LogVerbose("Seamless mode changing to %d", seamlessMode);

    if (g_SeamlessMode == seamlessMode && !forceUpdate)
        return ERROR_SUCCESS; // nothing to do

    CfgWriteDword(NULL, REG_CONFIG_SEAMLESS_VALUE, seamlessMode, NULL);

    if (!seamlessMode)
    {
        // show the screen window
        status = SendWindowMap(NULL);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowMap(NULL)");
    }
    else // seamless mode
    {
        // change the resolution to match host, if different
        if (g_ScreenWidth != g_HostScreenWidth || g_ScreenHeight != g_HostScreenHeight)
        {
            LogDebug("Changing resolution to match host's");
            RequestResolutionChange(g_HostScreenWidth, g_HostScreenHeight, 32, 0, 0);
            // FIXME: wait until the resolution actually changes?
        }
        // hide the screen window
        status = SendWindowUnmap(NULL);
        if (ERROR_SUCCESS != status)
            return perror2(status, "SendWindowUnmap(NULL)");
    }

    // ResetWatch kills hooks if active and removes all watched windows.
    // If seamless mode is on, hooks are restarted and top-level windows are added to watch list.
    status = ResetWatch(seamlessMode);
    if (ERROR_SUCCESS != status)
        return perror2(status, "ResetWatch");

    g_SeamlessMode = seamlessMode;

    LogInfo("Seamless mode changed to %d", seamlessMode);

    return ERROR_SUCCESS;
}

WINDOW_DATA *FindWindowByHandle(IN HWND window)
{
    WINDOW_DATA *watchedDC;

    LogVerbose("%x", window);
    watchedDC = (WINDOW_DATA *) g_WatchedWindowsList.Flink;
    while (watchedDC != (WINDOW_DATA *) &g_WatchedWindowsList)
    {
        watchedDC = CONTAINING_RECORD(watchedDC, WINDOW_DATA, ListEntry);

        if (window == watchedDC->WindowHandle)
            return watchedDC;

        watchedDC = (WINDOW_DATA *) watchedDC->ListEntry.Flink;
    }

    return NULL;
}

BOOL ShouldAcceptWindow(IN HWND window, IN const WINDOWINFO *windowInfo OPTIONAL)
{
    WINDOWINFO wi;

    if (!IsWindow(window))
        return FALSE;

    if (!windowInfo)
    {
        if (!GetWindowInfo(window, &wi))
        {
            perror("GetWindowInfo");
            return FALSE;
        }
        windowInfo = &wi;
    }

    if (g_bannedWindows.Explorer == window ||
        g_bannedWindows.Desktop == window ||
        g_bannedWindows.Start == window ||
        g_bannedWindows.Taskbar == window
        )
        return FALSE;

    // empty window rectangle? ignore (guid rejects those)
    if ((windowInfo->rcWindow.bottom - windowInfo->rcWindow.top == 0) || (windowInfo->rcWindow.right - windowInfo->rcWindow.left == 0))
    {
        LogVerbose("window rectangle is empty");
        return ERROR_SUCCESS;
    }

    // Ignore child windows, they are confined to parent's client area and can't be top-level.
    if (windowInfo->dwStyle & WS_CHILD)
        return FALSE;

    // Skip invisible windows.
    if (!IsWindowVisible(window))
        return FALSE;

    // Office 2013 uses this style for some helper windows that are drawn on/near its border.
    // 0x800 exstyle is undocumented...
    // FIXME: ignoring these border "windows" causes weird window looks.
    // Investigate why moving main Office window doesn't move these windows.
    if (windowInfo->dwExStyle == (WS_EX_LAYERED | WS_EX_TOOLWINDOW | 0x800))
        return FALSE;

    return TRUE;
}

// Enumerate top-level windows, searching for one that is modal
// in relation to a parent one (passed in lParam).
static BOOL WINAPI FindModalChildProc(IN HWND hwnd, IN LPARAM lParam)
{
    MODAL_SEARCH_PARAMS *msp = (MODAL_SEARCH_PARAMS *) lParam;
    LONG wantedStyle = WS_POPUP | WS_VISIBLE;
    HWND owner = GetWindow(hwnd, GW_OWNER);

    // Modal windows are not child windows but owned windows.
    if (owner != msp->ParentWindow)
        return TRUE;

    if ((GetWindowLong(hwnd, GWL_STYLE) & wantedStyle) != wantedStyle)
        return TRUE;

    msp->ModalWindow = hwnd;
    LogVerbose("0x%x: seems OK for 0x%x", hwnd, msp->ParentWindow);
    return FALSE; // stop enumeration
}

// Refresh data about a window, send notifications to guid if needed.
static ULONG UpdateWindowData(IN OUT WINDOW_DATA *wd, OUT BOOL *skip)
{
    ULONG status;
    WINDOWINFO wi = { 0 };
    int x, y, width, height;
    BOOL updateNeeded;
    WCHAR caption[256];
    MODAL_SEARCH_PARAMS modalParams = { 0 };

    *skip = FALSE;

    // get current window state
    wi.cbSize = sizeof(wi);

    if (!GetWindowInfo(wd->WindowHandle, &wi) || !ShouldAcceptWindow(wd->WindowHandle, &wi))
    {
        LogDebug("window %x disappeared, removing", wd->WindowHandle);
        status = RemoveWindow(wd);
        *skip = TRUE;
        return status;
    }

    // iconic state
    if (IsIconic(wd->WindowHandle))
    {
        if (!wd->IsIconic)
        {
            LogDebug("minimizing %x", wd->WindowHandle);
            wd->IsIconic = TRUE;
            *skip = TRUE;
            return SendWindowFlags(wd->WindowHandle, WINDOW_FLAG_MINIMIZE, 0); // exit
        }
        return ERROR_SUCCESS;
    }
    else
    {
        LogVerbose("%x not iconic", wd->WindowHandle);
        wd->IsIconic = FALSE;
    }

    // coords
    x = wi.rcWindow.left;
    y = wi.rcWindow.top;
    width = wi.rcWindow.right - wi.rcWindow.left;
    height = wi.rcWindow.bottom - wi.rcWindow.top;

    updateNeeded = (x != wd->X || y != wd->Y || width != wd->Width || height != wd->Height);

    wd->X = x;
    wd->Y = y;
    wd->Width = width;
    wd->Height = height;
    LogVerbose("%x (%d,%d) %dx%d", wd->WindowHandle, x, y, width, height);

    if (updateNeeded)
        SendWindowConfigure(wd);

    // visibility
    if (!IsWindowVisible(wd->WindowHandle))
    {
        LogDebug("window %x turned invisible", wd->WindowHandle);
        status = RemoveWindow(wd);
        *skip = TRUE;
        return status;
    }

    // caption
    GetWindowText(wd->WindowHandle, caption, RTL_NUMBER_OF(caption));
    if (0 != wcscmp(wd->Caption, caption))
    {
        // caption changed
        StringCchCopy(wd->Caption, RTL_NUMBER_OF(wd->Caption), caption);
        SendWindowName(wd->WindowHandle, wd->Caption);
    }

    // style
    if (wi.dwStyle & WS_DISABLED)
    {
        // possibly showing a modal window
        LogDebug("0x%x is WS_DISABLED, searching for modal window", wd->WindowHandle);
        modalParams.ParentWindow = wd->WindowHandle;
        modalParams.ModalWindow = NULL;

        // No checking for success, EnumWindows returns FALSE if the callback function returns FALSE.
        EnumWindows(FindModalChildProc, (LPARAM) &modalParams);

        LogDebug("result: 0x%x", modalParams.ModalWindow);
        if (modalParams.ModalWindow) // found a modal "child"
        {
            WINDOW_DATA *modalWindow = FindWindowByHandle(modalParams.ModalWindow);
            if (modalWindow && !modalWindow->ModalParent)
            {
                // need to toggle map since this is the only way to change modal status for gui daemon
                modalWindow->ModalParent = wd->WindowHandle;
                status = SendWindowUnmap(modalWindow->WindowHandle);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowUnmap");

                status = SendWindowMap(modalWindow);
                if (ERROR_SUCCESS != status)
                    return perror2(status, "SendWindowMap");
            }
        }
    }

    return ERROR_SUCCESS;
}

// Called after receiving screen damage event from qvideo.
static ULONG ProcessUpdatedWindows(IN HDC screenDC)
{
    WINDOW_DATA *entry;
    WINDOW_DATA *nextEntry;
    HWND oldDesktopWindow = g_DesktopWindow;
    ULONG totalPages, page, dirtyPages = 0;
    RECT dirtyArea, currentArea;
    BOOL first = TRUE;
    ULONG status = ERROR_SUCCESS;

    if (g_UseDirtyBits)
    {
        totalPages = g_ScreenHeight * g_ScreenWidth * 4 / PAGE_SIZE;
        // create a damage rectangle from changed pages
        for (page = 0; page < totalPages; page++)
        {
            if (BIT_GET(g_DirtyPages->DirtyBits, page))
            {
                dirtyPages++;
                PageToRect(page, &currentArea);
                if (first)
                {
                    dirtyArea = currentArea;
                    first = FALSE;
                }
                else
                    UnionRect(&dirtyArea, &dirtyArea, &currentArea);
            }
        }

        // tell qvideo that we're done reading dirty bits
        SynchronizeDirtyBits(screenDC);

        LogDebug("DIRTY %d/%d (%d,%d)-(%d,%d)", dirtyPages, totalPages,
            dirtyArea.left, dirtyArea.top, dirtyArea.right, dirtyArea.bottom);

        if (dirtyPages == 0) // nothing changed according to qvideo
            return ERROR_SUCCESS;
    }

    AttachToInputDesktop();
    if (oldDesktopWindow != g_DesktopWindow)
    {
        LogDebug("desktop changed (old 0x%x), refreshing all windows", oldDesktopWindow);
        oldDesktopWindow = g_DesktopWindow; // remember current desktop, ResetWatch clears it
        ResetWatch(g_SeamlessMode); // need to reinitialize hooks since they are confined to a desktop
        g_DesktopWindow = oldDesktopWindow;
        HideCursors();
        DisableEffects();
    }

    if (!g_SeamlessMode)
    {
        // just send damage event with the dirty area
        if (g_UseDirtyBits)
            SendWindowDamageEvent(0, dirtyArea.left, dirtyArea.top,
            dirtyArea.right - dirtyArea.left,
            dirtyArea.bottom - dirtyArea.top);
        else
            SendWindowDamageEvent(0, 0, 0, g_ScreenWidth, g_ScreenHeight);
        // TODO? if we're not using dirty bits we could narrow the damage area
        // by checking all windows... but it's probably not worth it.

        return ERROR_SUCCESS;
    }

    // Update the window list - check for new/destroyed windows.
    AddAllWindows();

    // Update all watched windows.
    EnterCriticalSection(&g_csWatchedWindows);

    entry = (WINDOW_DATA *) g_WatchedWindowsList.Flink;
    while (entry != (WINDOW_DATA *) &g_WatchedWindowsList)
    {
        BOOL skip;

        entry = CONTAINING_RECORD(entry, WINDOW_DATA, ListEntry);
        nextEntry = (WINDOW_DATA *) entry->ListEntry.Flink;

        UpdateWindowData(entry, &skip);
        if (skip)
        {
            entry = nextEntry;
            continue;
        }

        if (g_UseDirtyBits)
        {
            RECT entryRect = { entry->X, entry->Y, entry->X + entry->Width, entry->Y + entry->Width };

            // skip windows that aren't in the changed area
            if (IntersectRect(&currentArea, &dirtyArea, &entryRect))
            {
                status = SendWindowDamageEvent(entry->WindowHandle,
                    currentArea.left - entry->X, // TODO: verify
                    currentArea.top - entry->Y,
                    currentArea.right - entry->X,
                    currentArea.bottom - entry->Y);

                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendWindowDamageEvent");
                    goto cleanup;
                }
            }
        }
        else
        {
            // assume the whole window area changed
            status = SendWindowDamageEvent(entry->WindowHandle,
                0, 0, entry->Width, entry->Height);

            if (ERROR_SUCCESS != status)
            {
                perror2(status, "SendWindowDamageEvent");
                goto cleanup;
            }
        }

        entry = nextEntry;
    }

cleanup:
    LeaveCriticalSection(&g_csWatchedWindows);

    return status;
}

// main event loop
// TODO: refactor into smaller parts
static ULONG WINAPI WatchForEvents(void)
{
    HANDLE vchan;
    OVERLAPPED vchanAsyncState = { 0 };
    unsigned int firedPort;
    ULONG eventCount;
    DWORD i, signaledEvent;
    BOOL vchanIoInProgress;
    ULONG status;
    BOOL exitLoop;
    HANDLE watchedEvents[MAXIMUM_WAIT_OBJECTS];
    HANDLE windowDamageEvent, fullScreenOnEvent, fullScreenOffEvent;
    HDC screenDC;
    LARGE_INTEGER maxInterval, time1, time2;
    BOOL updatePending = FALSE;
#ifdef DEBUG
    DWORD dumpLastTime = GetTickCount();
    UINT64 damageCount = 0, damageCountOld = 0;
#endif

    LogDebug("start");
    windowDamageEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    // This will not block.
    if (!VchanInitServer(6000))
    {
        LogError("VchanInitServer() failed");
        return GetLastError();
    }

    vchan = VchanGetHandle();

    vchanAsyncState.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

    fullScreenOnEvent = CreateNamedEvent(FULLSCREEN_ON_EVENT_NAME);
    if (!fullScreenOnEvent)
        return GetLastError();
    fullScreenOffEvent = CreateNamedEvent(FULLSCREEN_OFF_EVENT_NAME);
    if (!fullScreenOffEvent)
        return GetLastError();
    g_ResolutionChangeEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!g_ResolutionChangeEvent)
        return GetLastError();

    if (g_MaxFps > 0)
    {
        QueryPerformanceFrequency(&maxInterval);
        LogDebug("frequency: %llu", maxInterval.QuadPart);
        maxInterval.QuadPart /= g_MaxFps;
        LogDebug("MaxFps: %lu, maxInterval: %llu", g_MaxFps, maxInterval.QuadPart);
        QueryPerformanceCounter(&time1);
    }

    g_VchanClientConnected = FALSE;
    vchanIoInProgress = FALSE;
    exitLoop = FALSE;

    LogInfo("Awaiting for a vchan client, write buffer size: %d", VchanGetWriteBufferSize());

    while (TRUE)
    {
        watchedEvents[0] = g_ShutdownEvent;
        watchedEvents[1] = windowDamageEvent;
        watchedEvents[2] = fullScreenOnEvent;
        watchedEvents[3] = fullScreenOffEvent;
        watchedEvents[4] = g_ResolutionChangeEvent;

        status = ERROR_SUCCESS;

        VchanPrepareToSelect();
        // read 1 byte instead of sizeof(fired_port) to not flush fired port
        // from evtchn buffer; evtchn driver will read only whole fired port
        // numbers (sizeof(fired_port)), so this will end in zero-length read
        if (!vchanIoInProgress && !ReadFile(vchan, &firedPort, 1, NULL, &vchanAsyncState))
        {
            status = GetLastError();
            if (ERROR_IO_PENDING != status)
            {
                perror("ReadFile");
                exitLoop = TRUE;
                break;
            }
        }

        vchanIoInProgress = TRUE;

        watchedEvents[5] = vchanAsyncState.hEvent;
        watchedEvents[6] = CreateEvent(NULL, FALSE, FALSE, NULL); // force update event
        eventCount = 7;

        // Wait for events.
        signaledEvent = WaitForMultipleObjects(eventCount, watchedEvents, FALSE, INFINITE);
        if (signaledEvent >= MAXIMUM_WAIT_OBJECTS)
        {
            status = perror("WaitForMultipleObjects");
            break;
        }

#ifdef DEBUG
        // dump watched windows every second
        if (GetTickCount() - dumpLastTime > 1000)
        {
            DumpWindows();
            dumpLastTime = GetTickCount();

            // dump performance counters
            LogDebug("last second damages: %llu", damageCount - damageCountOld);
            damageCountOld = damageCount;
        }
#endif

        if (0 == signaledEvent)
        {
            // shutdown event
            LogDebug("Shutdown event signaled");
            exitLoop = TRUE;
            break;
        }

        switch (signaledEvent)
        {
        case 6:
            LogVerbose("forced update");
            QueryPerformanceCounter(&time2);
            goto force_update;
            break;

        case 1: // damage event
            if (g_VchanClientConnected)
            {
                if (g_MaxFps > 0)
                {
                    QueryPerformanceCounter(&time2);
                    if (time2.QuadPart - time1.QuadPart < maxInterval.QuadPart) // fps throttling
                    {
                        if (!updatePending)
                        {
                            updatePending = TRUE;
                            // fire a delayed damage event to ensure we won't miss anything in case no damages follow
                            if (!timeSetEvent(1000 / g_MaxFps, 0, (LPTIMECALLBACK) watchedEvents[6], 0, TIME_ONESHOT | TIME_CALLBACK_EVENT_SET))
                                perror("timeSetEvent");
                        }
                        continue;
                    }
                force_update:
                    time1 = time2;
                    updatePending = FALSE;
                }
#ifdef DEBUG
                LogVerbose("Damage %llu", damageCount++);
#endif
                ProcessUpdatedWindows(screenDC);
            }
            break;

        case 2: // seamless off event
            status = SetSeamlessMode(FALSE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "SetSeamlessMode(FALSE)");
                exitLoop = TRUE;
            }
            break;

        case 3: // seamless on event
            status = SetSeamlessMode(TRUE, FALSE);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "SetSeamlessMode(TRUE)");
                exitLoop = TRUE;
            }
            break;

        case 4: // resolution change event, signaled by ResolutionChangeThread
            // Params are in g_ResolutionChangeParams
            status = ChangeResolution(&screenDC, windowDamageEvent);
            if (ERROR_SUCCESS != status)
            {
                perror2(status, "ChangeResolution");
                exitLoop = TRUE;
            }
            break;

        case 5: // vchan receive
            // the following will never block; we need to do this to
            // clear libvchan_fd pending state
            //
            // using libvchan_wait here instead of reading fired
            // port at the beginning of the loop (ReadFile call) to be
            // sure that we clear pending state _only_
            // when handling vchan data in this loop iteration (not any
            // other process)
            if (!g_VchanClientConnected)
            {
                VchanWait();

                vchanIoInProgress = FALSE;

                LogInfo("A vchan client has connected\n");

                // Remove the xenstore device/vchan/N entry.
                if (!VchanIsServerConnected())
                {
                    LogError("VchanIsServerConnected() failed");
                    exitLoop = TRUE;
                    break;
                }

                // needs to be set before enumerating windows so maps get sent
                // (and before sending anything really)
                g_VchanClientConnected = TRUE;

                if (ERROR_SUCCESS != SendProtocolVersion())
                {
                    LogError("SendProtocolVersion failed");
                    exitLoop = TRUE;
                    break;
                }

                // This will probably change the current video mode.
                if (ERROR_SUCCESS != HandleXconf())
                {
                    LogError("HandleXconf failed");
                    exitLoop = TRUE;
                    break;
                }

                // The screen DC should be opened only after the resolution changes.
                screenDC = GetDC(NULL);
                status = RegisterWatchedDC(screenDC, windowDamageEvent);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "RegisterWatchedDC");
                    exitLoop = TRUE;
                    break;
                }

                // send the whole screen framebuffer map
                status = SendWindowCreate(NULL);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendWindowCreate(NULL)");
                    exitLoop = TRUE;
                    break;
                }

                status = SendScreenMfns();
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SendWindowMfns(NULL)");
                    exitLoop = TRUE;
                    break;
                }

                // This initializes watched windows list.
                status = SetSeamlessMode(g_SeamlessMode, TRUE);
                if (ERROR_SUCCESS != status)
                {
                    perror2(status, "SetSeamlessMode");
                    exitLoop = TRUE;
                    break;
                }

                break;
            }

            if (!GetOverlappedResult(vchan, &vchanAsyncState, &i, FALSE))
            {
                if (GetLastError() == ERROR_IO_DEVICE)
                {
                    // in case of ring overflow, libvchan_wait
                    // will reset the evtchn ring, so ignore this
                    // error as already handled
                    //
                    // Overflow can happen when below loop ("while
                    // (read_ready_vchan_ext())") handle a lot of data
                    // in the same time as qrexec-daemon writes it -
                    // there where be no libvchan_wait call (which
                    // receive the events from the ring), but one will
                    // be signaled after each libvchan_write in
                    // qrexec-daemon. I don't know how to fix it
                    // properly (without introducing any race
                    // condition), so reset the evtchn ring (do not
                    // confuse with vchan ring, which stays untouched)
                    // in case of overflow.
                }
                else
                {
                    if (GetLastError() != ERROR_OPERATION_ABORTED)
                    {
                        perror("GetOverlappedResult(evtchn)");
                        exitLoop = TRUE;
                        break;
                    }
                }
            }

            EnterCriticalSection(&g_VchanCriticalSection);
            VchanWait();

            vchanIoInProgress = FALSE;

            if (VchanIsEof())
            {
                LogError("vchan disconnected");
                exitLoop = TRUE;
                LeaveCriticalSection(&g_VchanCriticalSection);
                break;
            }

            while (VchanGetReadBufferSize() > 0)
            {
                status = HandleServerData();
                if (ERROR_SUCCESS != status)
                {
                    exitLoop = TRUE;
                    LogError("HandleServerData failed: 0x%x", status);
                    break;
                }
            }
            LeaveCriticalSection(&g_VchanCriticalSection);

            break;
    }

        if (exitLoop)
            break;
}

    LogDebug("main loop finished");

    if (vchanIoInProgress)
    {
        if (CancelIo(vchan))
        {
            // Must wait for the canceled IO to complete, otherwise a race condition may occur on the
            // OVERLAPPED structure.
            LogDebug("Waiting for vchan operations to finish");
            WaitForSingleObject(vchanAsyncState.hEvent, INFINITE);
        }
    }

    if (!g_VchanClientConnected)
    {
        // Remove the xenstore device/vchan/N entry.
        LogDebug("cleaning up");
        VchanIsServerConnected();
    }

    if (g_VchanClientConnected)
        VchanClose();

    //StopHooks(&g_HookData); // don't care if it fails at this point

    CloseHandle(vchanAsyncState.hEvent);
    CloseHandle(windowDamageEvent);

    UnregisterWatchedDC(screenDC);
    CloseScreenSection();
    ReleaseDC(NULL, screenDC);
    LogInfo("exiting");

    return exitLoop ? ERROR_INVALID_FUNCTION : ERROR_SUCCESS;
}

static DWORD GetDomainName(OUT char *nameBuffer, IN DWORD nameLength)
{
    DWORD status = ERROR_SUCCESS;
    struct xs_handle *xs;
    char *domainName = NULL;

    xs = xs_domain_open();
    if (!xs)
    {
        LogError("Failed to open xenstore connection");
        status = ERROR_DEVICE_NOT_CONNECTED;
        goto cleanup;
    }

    domainName = xs_read(xs, XBT_NULL, "name", NULL);
    if (!domainName)
    {
        LogError("Failed to read domain name");
        status = ERROR_NOT_FOUND;
        goto cleanup;
    }

    LogDebug("%S", domainName);
    status = StringCchCopyA(nameBuffer, nameLength, domainName);
    if (FAILED(status))
    {
        perror2(status, "StringCchCopyA");
    }

cleanup:
    free(domainName);
    if (xs)
        xs_daemon_close(xs);

    return status;
}

static ULONG Init(void)
{
    ULONG status;
    WSADATA wsaData;
    WCHAR moduleName[CFG_MODULE_MAX];

    LogDebug("start");

    // This needs to be done first as a safeguard to not start multiple instances of this process.
    g_ShutdownEvent = CreateNamedEvent(WGA_SHUTDOWN_EVENT_NAME);
    if (!g_ShutdownEvent)
    {
        return GetLastError();
    }

    status = CfgGetModuleName(moduleName, RTL_NUMBER_OF(moduleName));

    status = CfgReadDword(moduleName, REG_CONFIG_DIRTY_VALUE, &g_UseDirtyBits, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, disabling that feature", REG_CONFIG_DIRTY_VALUE);
        g_UseDirtyBits = FALSE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_CURSOR_VALUE, &g_DisableCursor, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_CURSOR_VALUE);
        g_DisableCursor = TRUE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_SEAMLESS_VALUE, &g_SeamlessMode, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (TRUE)", REG_CONFIG_SEAMLESS_VALUE);
        g_SeamlessMode = TRUE;
    }

    status = CfgReadDword(moduleName, REG_CONFIG_FPS_VALUE, &g_MaxFps, NULL);
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read '%s' config value, using default (0)", REG_CONFIG_FPS_VALUE);
        g_MaxFps = 0;
    }
    LogInfo("MaxFps: %lu", g_MaxFps);

    SystemParametersInfo(SPI_SETFOREGROUNDLOCKTIMEOUT, 0, 0, SPIF_UPDATEINIFILE);

    HideCursors();
    DisableEffects();

    status = IncreaseProcessWorkingSetSize(1024 * 1024 * 100, 1024 * 1024 * 1024);
    if (ERROR_SUCCESS != status)
    {
        perror("IncreaseProcessWorkingSetSize");
        // try to continue
    }

    SetLastError(status = CheckForXenInterface());
    if (ERROR_SUCCESS != status)
    {
        return perror("CheckForXenInterface");
    }

    // Read domain name from xenstore.
    status = GetDomainName(g_DomainName, RTL_NUMBER_OF(g_DomainName));
    if (ERROR_SUCCESS != status)
    {
        LogWarning("Failed to read domain name from xenstore, using host name");

        status = WSAStartup(MAKEWORD(2, 2), &wsaData);
        if (status == 0)
        {
            if (0 != gethostname(g_DomainName, sizeof(g_DomainName)))
            {
                LogWarning("gethostname failed: 0x%x", status);
            }
            WSACleanup();
        }
        else
        {
            LogWarning("WSAStartup failed: 0x%x", status);
            // this is not fatal, only used to get host name for full desktop window title
        }
    }

    LogInfo("Fullscreen desktop name: %S", g_DomainName);

    InitializeListHead(&g_WatchedWindowsList);
    InitializeCriticalSection(&g_csWatchedWindows);
    return ERROR_SUCCESS;
}

int wmain(int argc, WCHAR *argv[])
{
    if (ERROR_SUCCESS != Init())
        return perror("Init");

    InitializeCriticalSection(&g_VchanCriticalSection);

    // Call the thread proc directly.
    if (ERROR_SUCCESS != WatchForEvents())
        return perror("WatchForEvents");

    DeleteCriticalSection(&g_VchanCriticalSection);

    LogInfo("exiting");
    return ERROR_SUCCESS;
}
