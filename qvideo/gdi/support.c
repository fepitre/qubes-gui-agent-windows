#include <wdm.h>
#include "common.h"
#include "support.h"
#include "mi.h"

LARGE_INTEGER g_PCFrequency;
LARGE_INTEGER g_RefreshInterval;
ULONG g_MaxFps = DEFAULT_MAX_REFRESH_FPS;
BOOLEAN g_bUseDirtyBits = TRUE;

ULONG CfgReadDword(IN PWCHAR valueName, OUT ULONG *value)
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    HANDLE handleRegKey = NULL;
    OBJECT_ATTRIBUTES attributes;
    UNICODE_STRING usKeyName;
    UNICODE_STRING usValueName;
    PKEY_VALUE_FULL_INFORMATION pKeyInfo = NULL;
    ULONG ulKeyInfoSize = 0;
    ULONG ulKeyInfoSizeNeeded = 0;

    RtlInitUnicodeString(&usKeyName, REG_CONFIG_KERNEL_KEY);
    InitializeObjectAttributes(&attributes, 
        &usKeyName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,    // handle
        NULL);

    status = ZwOpenKey(&handleRegKey, KEY_READ, &attributes);
    if (!NT_SUCCESS(status)) 
    {
        WARNINGF("ZwOpenKey(%wZ) failed", usKeyName);
        goto cleanup;
    }

    RtlInitUnicodeString(&usValueName, valueName);

    // Determine the required size of keyInfo.
    status = ZwQueryValueKey(
        handleRegKey,
        &usValueName,
        KeyValueFullInformation,
        pKeyInfo,
        ulKeyInfoSize,
        &ulKeyInfoSizeNeeded);

    if ((status == STATUS_BUFFER_TOO_SMALL) || (status == STATUS_BUFFER_OVERFLOW))
    {
        ulKeyInfoSize = ulKeyInfoSizeNeeded;
        pKeyInfo = (PKEY_VALUE_FULL_INFORMATION) ExAllocatePoolWithTag(NonPagedPool, ulKeyInfoSizeNeeded, QVDISPLAY_TAG);
        if (NULL == pKeyInfo)
        {
            ERRORF("No memory");
            goto cleanup;
        }

        RtlZeroMemory(pKeyInfo, ulKeyInfoSize);
        // Get the key data.
        status = ZwQueryValueKey(
            handleRegKey,
            &usValueName,
            KeyValueFullInformation,
            pKeyInfo,
            ulKeyInfoSize,
            &ulKeyInfoSizeNeeded);

        if ((status != STATUS_SUCCESS) || (ulKeyInfoSizeNeeded != ulKeyInfoSize) || (NULL == pKeyInfo))
        {
            WARNINGF("ZwQueryValueKey(%wZ) failed: 0x%x", usValueName, status);
            goto cleanup;
        }

        if (pKeyInfo->Type != REG_DWORD)
        {
            WARNINGF("config value '%wZ' is not DWORD but 0x%x", usValueName, pKeyInfo->Type);
            goto cleanup;
        }

        RtlCopyMemory(value, (PUCHAR) pKeyInfo + pKeyInfo->DataOffset, sizeof(ULONG));
    }

    status = STATUS_SUCCESS;

cleanup:
    if (pKeyInfo)
        ExFreePoolWithTag(pKeyInfo, QVDISPLAY_TAG);

    if (handleRegKey)
        ZwClose(handleRegKey);

    return status;
}

VOID ReadRegistryConfig()
{
    static BOOLEAN bInitialized = FALSE;
    ULONG ulUseDirtyBits = g_bUseDirtyBits;

    if (bInitialized)
        return;

    // frequency doesn't change, query it once
    KeQueryPerformanceCounter(&g_PCFrequency);

    // read maximum refresh FPS
    if (!NT_SUCCESS(CfgReadDword(REG_CONFIG_FPS_VALUE, &g_MaxFps)))
    {
        g_MaxFps = DEFAULT_MAX_REFRESH_FPS;
        WARNINGF("failed to read '%s' config value, using %lu", REG_CONFIG_FPS_VALUE, g_MaxFps);
    }

    if (g_MaxFps > MAX_REFRESH_FPS)
    {
        WARNINGF("invalid refresh FPS: %lu, reverting to default %lu", g_MaxFps, DEFAULT_MAX_REFRESH_FPS);
        g_MaxFps = DEFAULT_MAX_REFRESH_FPS;
    }

    if (g_MaxFps != 0)
    {
        g_RefreshInterval.QuadPart = g_PCFrequency.QuadPart / g_MaxFps;
        DEBUGF("FPS: %lu, freq: %I64d, interval: %I64d", g_MaxFps, g_PCFrequency.QuadPart, g_RefreshInterval.QuadPart);
    }
    else
    {
        DEBUGF("FPS limit disabled");
    }

    // dirty bits
    if (!NT_SUCCESS(CfgReadDword(REG_CONFIG_DIRTY_VALUE, &ulUseDirtyBits)))
    {
        WARNINGF("failed to read '%s' config value, using %lu", REG_CONFIG_DIRTY_VALUE, g_bUseDirtyBits);
    }
    else
    {
        g_bUseDirtyBits = (BOOLEAN) ulUseDirtyBits;
        DEBUGF("%s: %lu", REG_CONFIG_DIRTY_VALUE, ulUseDirtyBits);
    }

    bInitialized = TRUE;
}

// debug
VOID DumpPte(PVOID va, PMMPTE pte)
{
    UNREFERENCED_PARAMETER(va);
    UNREFERENCED_PARAMETER(pte);

    DEBUGF("VA=%p PTE=%lx %s%s%s%s%s%s%s%s%s%s%s%s",
        va, pte->u.Long,
        pte->u.Hard.Valid ? "" : "INVALID ",
        pte->u.Hard.LargePage ? "Large " : "",
        pte->u.Hard.Global ? "Global " : "",
        pte->u.Hard.Owner ? "User " : "Kernel ",
        pte->u.Hard.NoExecute ? "NX " : "",
        pte->u.Hard.Prototype ? "Proto " : "",
        pte->u.Hard.CacheDisable ? "CacheDisable " : "",
        pte->u.Hard.CopyOnWrite ? "COW " : "",
        pte->u.Hard.WriteThrough ? "WT " : "",
        pte->u.Hard.Write ? "Write " : "",
        pte->u.Hard.Dirty ? "Dirty " : "",
        pte->u.Hard.Accessed ? "Accessed" : ""
        );
}

// returns number of changed pages
ULONG UpdateDirtyBits(
    PVOID va,
    ULONG size,
    PQV_DIRTY_PAGES pDirtyPages,
    IN OUT PLARGE_INTEGER pTimestamp
    )
{
    LARGE_INTEGER timestamp;
    ULONG pages, pageNumber, dirty = 0;
    PUCHAR ptr;
    PMMPTE pte;
#ifdef DBG
    static ULONG counter = 0;
    LARGE_INTEGER stime, ltime;
    TIME_FIELDS tf;
#endif

    if (g_MaxFps != 0) // FPS limiter enabled
    {
        timestamp = KeQueryPerformanceCounter(NULL);
        //DEBUGF("ts: %I64d", timestamp.QuadPart);
        if (timestamp.QuadPart < pTimestamp->QuadPart + g_RefreshInterval.QuadPart)
            return 0; // too soon

        *pTimestamp = timestamp;
    }

    if (!g_bUseDirtyBits)
        return 1; // just signal the refresh event

    pages = size / PAGE_SIZE;

    // ready == 1 means that the client has read all the data, we can reset the bit field
    // and set ready to 0
    if (InterlockedCompareExchange(&pDirtyPages->Ready, 0, 1) == 1)
    {
        RtlZeroMemory(pDirtyPages->DirtyBits, (pages >> 3) + 1);
        DEBUGF("WGA ready");
    }

    for (ptr = (PUCHAR)va, pageNumber = 0;
        ptr < (PUCHAR)va + size;
        ptr += PAGE_SIZE, pageNumber++
        )
    {
        // check PDE: if it's large, there is no PTE
        pte = MiGetPdeAddress(ptr);
        if (!IsPteLarge(pte))
            pte = MiGetPteAddress(ptr);
        //DumpPte(ptr, pte);
        //if (IsPteValid(pte)) // memory is locked, should be always valid
        {
            if (IsPteDirty(pte))
            {
                BIT_SET(pDirtyPages->DirtyBits, pageNumber);
                pte->u.Hard.Dirty = 0;         
                dirty++;
            }
        }
    }

#if DBG
    KeQuerySystemTime(&stime);
    ExSystemTimeToLocalTime(&stime, &ltime);
    RtlTimeToTimeFields(&ltime, &tf);
    DEBUGF("%02d%02d%02d.%03d %08lu: VA %p, %04d/%d",
        tf.Hour, tf.Minute, tf.Second, tf.Milliseconds,
        counter++, va, dirty, pages);
#endif

    return dirty;
}