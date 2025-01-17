#include "driver.h"
#include "support.h"

// The driver function table with all function index/address pairs
static DRVFN g_DrvFunctions[] =
{
    { INDEX_DrvGetModes, (PFN) DrvGetModes },
    { INDEX_DrvSynchronizeSurface, (PFN) DrvSynchronizeSurface },
    { INDEX_DrvEnablePDEV, (PFN) DrvEnablePDEV },
    { INDEX_DrvCompletePDEV, (PFN) DrvCompletePDEV },
    { INDEX_DrvDisablePDEV, (PFN) DrvDisablePDEV },
    { INDEX_DrvEnableSurface, (PFN) DrvEnableSurface },
    { INDEX_DrvDisableSurface, (PFN) DrvDisableSurface },
    { INDEX_DrvAssertMode, (PFN) DrvAssertMode },
    //{INDEX_DrvCreateDeviceBitmap, (PFN) DrvCreateDeviceBitmap},
    //{INDEX_DrvDeleteDeviceBitmap, (PFN) DrvDeleteDeviceBitmap},
    { INDEX_DrvEscape, (PFN) DrvEscape }
};

// default mode, before wga.exe connects
ULONG g_uDefaultWidth = 800;
ULONG g_uDefaultHeight = 600;
// initial second mode, will be updated by wga.exe based on dom0 mode
ULONG g_uWidth = 1280;
ULONG g_uHeight = 800;
ULONG g_uBpp = 32;

#define flGlobalHooks HOOK_SYNCHRONIZE

/******************************Public*Routine******************************\
* DrvEnableDriver
*
* Enables the driver by retrieving the drivers function table and version.
*
\**************************************************************************/
BOOL APIENTRY DrvEnableDriver(
    ULONG iEngineVersion,
    ULONG cj,
    __in_bcount(cj) DRVENABLEDATA *pded
    )
{
    DISPDBG((0, "DrvEnableDriver\n"));

    if ((iEngineVersion < DDI_DRIVER_VERSION_NT5) || (cj < sizeof(DRVENABLEDATA)))
    {
        DISPDBG((0, "DrvEnableDriver(): Unsupported engine version %d, DRVENABLEDATA size %d\n", iEngineVersion, cj));
        return FALSE;
    }

    pded->pdrvfn = g_DrvFunctions;
    pded->c = sizeof(g_DrvFunctions) / sizeof(DRVFN);
    pded->iDriverVersion = DDI_DRIVER_VERSION_NT5;

    ReadRegistryConfig();

    return TRUE;
}

/******************************Public*Routine******************************\
* DrvEnablePDEV
*
* DDI function, Enables the Physical Device.
*
* Return Value: device handle to pdev.
*
\**************************************************************************/

DHPDEV APIENTRY DrvEnablePDEV(
    __in DEVMODEW *pDevmode,	// Pointer to DEVMODE
    __in_opt WCHAR *pwszLogAddress,	// Logical address
    __in ULONG cPatterns,	// number of patterns
    __in_opt HSURF *ahsurfPatterns,	// return standard patterns
    __in ULONG cjGdiInfo,	// Length of memory pointed to by pGdiInfo
    __out_bcount(cjGdiInfo) ULONG *pGdiInfo,	// Pointer to GdiInfo structure
    __in ULONG cjDevInfo,	// Length of following PDEVINFO structure
    __out_bcount(cjDevInfo) DEVINFO *pDevInfo,	// physical device information structure
    __in_opt HDEV hdev,	// HDEV, used for callbacks
    __in_opt WCHAR *pwszDeviceName,	// DeviceName - not used
    __in HANDLE hDriver	// Handle to base driver
    )
{
    PDEV *ppdev = NULL;

    DISPDBG((0, "DrvEnablePDEV\n"));

    UNREFERENCED_PARAMETER(pwszLogAddress);
    UNREFERENCED_PARAMETER(cPatterns);
    UNREFERENCED_PARAMETER(ahsurfPatterns);
    UNREFERENCED_PARAMETER(hdev);
    UNREFERENCED_PARAMETER(pwszDeviceName);

    if (sizeof(DEVINFO) > cjDevInfo)
    {
        DISPDBG((0, "DrvEnablePDEV(): insufficient pDevInfo memory\n"));
        return NULL;
    }

    if (sizeof(GDIINFO) > cjGdiInfo)
    {
        DISPDBG((0, "DrvEnablePDEV(): insufficient pGdiInfo memory\n"));
        return NULL;
    }
    // Allocate a physical device structure.

    ppdev = (PDEV *) EngAllocMem(FL_ZERO_MEMORY, sizeof(PDEV), ALLOC_TAG);
    if (ppdev == NULL)
    {
        DISPDBG((0, "DrvEnablePDEV(): EngAllocMem() failed\n"));
        return NULL;
    }
    // Save the screen handle in the PDEV.

    ppdev->DriverHandle = hDriver;

    // Get the current screen mode information. Set up device caps and devinfo.

    if (!InitPdev(ppdev, pDevmode, (GDIINFO *) pGdiInfo, pDevInfo))
    {
        DISPDBG((0, "DrvEnablePDEV(): bInitPDEV() failed\n"));
        EngFreeMem(ppdev);
        return NULL;
    }

    return (DHPDEV) ppdev;
}

/******************************Public*Routine******************************\
* DrvCompletePDEV
*
* Store the HPDEV, the engines handle for this PDEV, in the DHPDEV.
*
\**************************************************************************/

VOID APIENTRY DrvCompletePDEV(
    DHPDEV dhpdev,
    HDEV hdev
    )
{
    DISPDBG((1, "DrvCompletePDEV\n"));
    ((PDEV *) dhpdev)->EngPdevHandle = hdev;
}

ULONG APIENTRY DrvGetModes(
    HANDLE hDriver,
    ULONG cjSize,
    DEVMODEW *pdm
    )
{
    ULONG ulBytesWritten = 0, ulBytesNeeded = 2 * sizeof(DEVMODEW);
    ULONG ulReturnValue;
    DWORD i;

    UNREFERENCED_PARAMETER(hDriver);
    UNREFERENCED_PARAMETER(cjSize);

    DISPDBG((1, "DrvGetModes(%p, %lu), bytes needed: %lu\n", pdm, cjSize, ulBytesNeeded));
    if (pdm == NULL)
    {
        ulReturnValue = ulBytesNeeded;
    }
    else if (cjSize < ulBytesNeeded)
    {
        ulReturnValue = 0;
    }
    else
    {
        ulBytesWritten = ulBytesNeeded;

        memset(pdm, 0, ulBytesNeeded);
        for (i = 0; i < 2; i++)
        {
            memcpy(pdm[i].dmDeviceName, DLL_NAME, sizeof(DLL_NAME));

            pdm[i].dmSpecVersion = DM_SPECVERSION;
            pdm[i].dmDriverVersion = DM_SPECVERSION;

            pdm[i].dmDriverExtra = 0;
            pdm[i].dmSize = sizeof(DEVMODEW);
            pdm[i].dmBitsPerPel = g_uBpp;
            switch (i)
            {
            case 0:
                pdm[i].dmPelsWidth = g_uDefaultWidth;
                pdm[i].dmPelsHeight = g_uDefaultHeight;
                break;
            case 1:
                pdm[i].dmPelsWidth = g_uWidth;
                pdm[i].dmPelsHeight = g_uHeight;
                break;
            }
            pdm[i].dmDisplayFrequency = 75;

            pdm[i].dmDisplayFlags = 0;

            pdm[i].dmPanningWidth = pdm[i].dmPelsWidth;
            pdm[i].dmPanningHeight = pdm[i].dmPelsHeight;

            pdm[i].dmFields = DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
        }

        ulReturnValue = ulBytesWritten;
    }

    DISPDBG((1, "DrvGetModes(): return %d\n", ulReturnValue));
    return ulReturnValue;
}

/******************************Public*Routine******************************\
* DrvDisablePDEV
*
* Release the resources allocated in DrvEnablePDEV. If a surface has been
* enabled DrvDisableSurface will have already been called.
*
\**************************************************************************/

VOID APIENTRY DrvDisablePDEV(
    DHPDEV dhpdev
    )
{
    PDEV *ppdev = (PDEV *) dhpdev;

    DISPDBG((1, "DrvDisablePDEV\n"));

    EngDeletePalette(ppdev->DefaultPalette);
    EngFreeMem(ppdev);
}

ULONG AllocateSurfaceMemory(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    QVMINI_ALLOCATE_MEMORY_RESPONSE *pQvminiAllocateMemoryResponse = NULL;
    QVMINI_ALLOCATE_MEMORY QvminiAllocateMemory;
    ULONG nbReturned;
    DWORD dwResult;

    pQvminiAllocateMemoryResponse = (QVMINI_ALLOCATE_MEMORY_RESPONSE *)
        EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE), ALLOC_TAG);

    if (!pQvminiAllocateMemoryResponse)
    {
        RIP("AllocateSurfaceMemory(): EngAllocMem(QVMINI_ALLOCATE_MEMORY_RESPONSE) failed\n");
        return STATUS_NO_MEMORY;
    }

    QvminiAllocateMemory.Size = uLength;

    dwResult = EngDeviceIoControl(
        pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_ALLOCATE_MEMORY,
        &QvminiAllocateMemory,
        sizeof(QvminiAllocateMemory),
        pQvminiAllocateMemoryResponse,
        sizeof(QVMINI_ALLOCATE_MEMORY_RESPONSE),
        &nbReturned);

    if (0 != dwResult)
    {
        DISPDBG((0, "AllocateSurfaceMemory(): EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_MEMORY) failed with error %d\n", dwResult));
        EngFreeMem(pQvminiAllocateMemoryResponse);
        return STATUS_NO_MEMORY;
    }

    pSurfaceDescriptor->SurfaceData = pQvminiAllocateMemoryResponse->VirtualAddress;
    pSurfaceDescriptor->pPfnArray = pQvminiAllocateMemoryResponse->PfnArray;

    EngFreeMem(pQvminiAllocateMemoryResponse);

    return 0;
}

ULONG AllocateSection(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    QVMINI_ALLOCATE_SECTION_RESPONSE *pQvminiAllocateSectionResponse = NULL;
    QVMINI_ALLOCATE_SECTION QvminiAllocateSection;
    ULONG nbReturned;
    DWORD dwResult;

    pQvminiAllocateSectionResponse = (QVMINI_ALLOCATE_SECTION_RESPONSE *)
        EngAllocMem(FL_ZERO_MEMORY, sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE), ALLOC_TAG);

    if (!pQvminiAllocateSectionResponse)
    {
        RIP("AllocateSection(): EngAllocMem(QVMINI_ALLOCATE_SECTION_RESPONSE) failed\n");
        return STATUS_NO_MEMORY;
    }

    QvminiAllocateSection.Size = uLength;
    QvminiAllocateSection.UseDirtyBits = g_bUseDirtyBits;

    dwResult = EngDeviceIoControl(
        pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_ALLOCATE_SECTION,
        &QvminiAllocateSection,
        sizeof(QvminiAllocateSection),
        pQvminiAllocateSectionResponse,
        sizeof(QVMINI_ALLOCATE_SECTION_RESPONSE),
        &nbReturned);

    if (0 != dwResult)
    {
        DISPDBG((0, "AllocateSection(): EngDeviceIoControl(IOCTL_QVMINI_ALLOCATE_SECTION) failed with error %d\n", dwResult));
        EngFreeMem(pQvminiAllocateSectionResponse);
        return STATUS_NO_MEMORY;
    }

    pSurfaceDescriptor->SurfaceData = pQvminiAllocateSectionResponse->VirtualAddress;
    pSurfaceDescriptor->SurfaceSection = pQvminiAllocateSectionResponse->Section;
    pSurfaceDescriptor->SectionObject = pQvminiAllocateSectionResponse->SectionObject;
    pSurfaceDescriptor->Mdl = pQvminiAllocateSectionResponse->Mdl;

    pSurfaceDescriptor->DirtySectionObject = pQvminiAllocateSectionResponse->DirtySectionObject;
    pSurfaceDescriptor->DirtySection = pQvminiAllocateSectionResponse->DirtySection;
    pSurfaceDescriptor->DirtyPages = pQvminiAllocateSectionResponse->DirtyPages;

    pSurfaceDescriptor->pPfnArray = pQvminiAllocateSectionResponse->PfnArray;

    EngFreeMem(pQvminiAllocateSectionResponse);

    return 0;
}

ULONG AllocateSurfaceData(
    BOOLEAN bSurface,
    SURFACE_DESCRIPTOR *pSurfaceDescriptor,
    ULONG uLength
    )
{
    if (bSurface)
        return AllocateSection(pSurfaceDescriptor, uLength);
    else
        return AllocateSurfaceMemory(pSurfaceDescriptor, uLength);
}

VOID FreeSurfaceMemory(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    DWORD dwResult;
    QVMINI_FREE_MEMORY QvminiFreeMemory;
    ULONG nbReturned;

    DISPDBG((0, "FreeSurfaceMemory(%p)\n", pSurfaceDescriptor->SurfaceData));

    QvminiFreeMemory.VirtualAddress = pSurfaceDescriptor->SurfaceData;
    QvminiFreeMemory.PfnArray = pSurfaceDescriptor->pPfnArray;

    dwResult = EngDeviceIoControl(pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_FREE_MEMORY, &QvminiFreeMemory, sizeof(QvminiFreeMemory), NULL, 0, &nbReturned);

    if (0 != dwResult)
    {
        DISPDBG((0, "FreeSurfaceMemory(): EngDeviceIoControl(IOCTL_QVMINI_FREE_MEMORY) failed with error %d\n", dwResult));
    }
}

VOID FreeSection(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    DWORD dwResult;
    QVMINI_FREE_SECTION QvminiFreeSection;
    ULONG nbReturned;

    DISPDBG((0, "FreeSection(%p)\n", pSurfaceDescriptor->SurfaceData));

    QvminiFreeSection.VirtualAddress = pSurfaceDescriptor->SurfaceData;
    QvminiFreeSection.Section = pSurfaceDescriptor->SurfaceSection;
    QvminiFreeSection.SectionObject = pSurfaceDescriptor->SectionObject;
    QvminiFreeSection.Mdl = pSurfaceDescriptor->Mdl;

    QvminiFreeSection.DirtyPages = pSurfaceDescriptor->DirtyPages;
    QvminiFreeSection.DirtySection = pSurfaceDescriptor->DirtySection;
    QvminiFreeSection.DirtySectionObject = pSurfaceDescriptor->DirtySectionObject;

    QvminiFreeSection.PfnArray = pSurfaceDescriptor->pPfnArray;

    dwResult = EngDeviceIoControl(pSurfaceDescriptor->Pdev->DriverHandle,
        IOCTL_QVMINI_FREE_SECTION, &QvminiFreeSection, sizeof(QvminiFreeSection), NULL, 0, &nbReturned);

    if (0 != dwResult)
    {
        DISPDBG((0, "FreeSection(): EngDeviceIoControl(IOCTL_QVMINI_FREE_SECTION) failed with error %d\n", dwResult));
    }
}

VOID FreeSurfaceData(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    if (pSurfaceDescriptor->IsScreen)
        FreeSection(pSurfaceDescriptor);
    else
        FreeSurfaceMemory(pSurfaceDescriptor);
}

VOID FreeSurfaceDescriptor(
    SURFACE_DESCRIPTOR *pSurfaceDescriptor
    )
{
    if (!pSurfaceDescriptor)
        return;

    if (pSurfaceDescriptor->DriverObj)
    {
        // Require a cleanup callback to be called.
        // pSurfaceDescriptor->hDriverObj will be set no NULL by the callback.
        EngDeleteDriverObj(pSurfaceDescriptor->DriverObj, TRUE, FALSE);
    }

    FreeSurfaceData(pSurfaceDescriptor);
    EngFreeMem(pSurfaceDescriptor);
}

ULONG AllocateNonOpaqueDeviceSurfaceOrBitmap(
    BOOLEAN bSurface,
    HDEV hdev,
    ULONG ulBitCount,
    SIZEL sizl,
    ULONG ulHooks,
    HSURF *pHsurf,
    SURFACE_DESCRIPTOR **ppSurfaceDescriptor,
    PDEV *ppdev
    )
{
    ULONG ulBitmapType;
    ULONG uSurfaceMemorySize;
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;
    ULONG uStride;
    HSURF hsurf;
    DHSURF dhsurf;
    ULONG uResult;

    if (!hdev || !pHsurf || !ppSurfaceDescriptor)
        return STATUS_INVALID_PARAMETER;

    switch (ulBitCount)
    {
    case 8:
        ulBitmapType = BMF_8BPP;
        break;
    case 16:
        ulBitmapType = BMF_16BPP;
        break;
    case 24:
        ulBitmapType = BMF_24BPP;
        break;
    case 32:
        ulBitmapType = BMF_32BPP;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    uStride = (ulBitCount >> 3) * sizl.cx;

    uSurfaceMemorySize = (ULONG) (uStride * sizl.cy);

    //    DISPDBG((0, "AllocateNonOpaqueDeviceSurfaceOrBitmap(): Allocating %dx%d, %d\n", sizl.cx, sizl.cy, ulBitCount));

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) EngAllocMem(FL_ZERO_MEMORY, sizeof(SURFACE_DESCRIPTOR), ALLOC_TAG);
    if (!pSurfaceDescriptor)
    {
        RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngAllocMem(SURFACE_DESCRIPTOR) failed\n");
        return STATUS_NO_MEMORY;
    }

    pSurfaceDescriptor->Pdev = ppdev;

    // pSurfaceDescriptor->ppdev must be set before calling this,
    // because we query our miniport through EngDeviceIoControl().
    uResult = AllocateSurfaceData(bSurface, pSurfaceDescriptor, uSurfaceMemorySize);
    if (0 != uResult)
    {
        DISPDBG((0, "AllocateNonOpaqueDeviceSurfaceOrBitmap(): AllocateSurfaceData(%d) failed with error %d\n", uSurfaceMemorySize, uResult));
        EngFreeMem(pSurfaceDescriptor);
        return uResult;
    }
    // Create a surface.

    dhsurf = (DHSURF) pSurfaceDescriptor;

    if (bSurface)
        hsurf = EngCreateDeviceSurface(dhsurf, sizl, ulBitmapType);
    else
        hsurf = (HSURF) EngCreateDeviceBitmap(dhsurf, sizl, ulBitmapType);

    if (!hsurf)
    {
        RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngCreateDevice*() failed\n");
        FreeSurfaceDescriptor(pSurfaceDescriptor);
        return STATUS_INVALID_HANDLE;
    }

    if (!EngModifySurface(hsurf, hdev, ulHooks, 0, (DHSURF) dhsurf, pSurfaceDescriptor->SurfaceData, uStride, NULL))
    {
        RIP("AllocateNonOpaqueDeviceSurfaceOrBitmap(): EngModifySurface() failed\n");
        EngDeleteSurface(hsurf);
        FreeSurfaceDescriptor(pSurfaceDescriptor);
        return STATUS_INVALID_HANDLE;
    }

    pSurfaceDescriptor->Width = sizl.cx;
    pSurfaceDescriptor->Height = sizl.cy;
    pSurfaceDescriptor->Delta = uStride;
    pSurfaceDescriptor->BitCount = ulBitCount;

    if (sizl.cx > 50)
    {
        //        DISPDBG((0, "Surface bitmap header %dx%d at %p (0x%x bytes), data at %p (0x%x bytes), pfns: %d\n", sizl.cx, sizl.cy,
        //            &pSurfaceDescriptor->BitmapHeader, sizeof(BitmapHeader), pSurfaceDescriptor->pSurfaceData, uSurfaceMemorySize,
        //            pSurfaceDescriptor->PfnArray.uNumberOf4kPages));
        DISPDBG((0, "Surface %dx%d, data at %p (0x%x bytes), pfns: %d\n",
            sizl.cx, sizl.cy, pSurfaceDescriptor->SurfaceData, uSurfaceMemorySize,
            pSurfaceDescriptor->pPfnArray->NumberOf4kPages));
        DISPDBG((0, "First phys pages: 0x%x, 0x%x, 0x%x\n",
            pSurfaceDescriptor->pPfnArray->Pfn[0], pSurfaceDescriptor->pPfnArray->Pfn[1],
            pSurfaceDescriptor->pPfnArray->Pfn[2]));
    }

    *ppSurfaceDescriptor = pSurfaceDescriptor;
    *pHsurf = hsurf;

    return 0;
}

/******************************Public*Routine******************************\
* DrvEnableSurface
*
* Enable the surface for the device. Hook the calls this driver supports.
*
* Return: Handle to the surface if successful, 0 for failure.
*
\**************************************************************************/

HSURF APIENTRY DrvEnableSurface(
    DHPDEV dhpdev
    )
{
    PDEV *ppdev;
    HSURF hsurf;
    SIZEL sizl;
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    LONG Status;

    // Create engine bitmap around frame buffer.

    DISPDBG((0, "DrvEnableSurface\n"));

    ppdev = (PDEV *) dhpdev;

    sizl.cx = ppdev->ScreenWidth;
    sizl.cy = ppdev->ScreenHeight;

    Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(
        TRUE, ppdev->EngPdevHandle, ppdev->BitsPerPel, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);

    if (Status < 0)
    {
        DISPDBG((0, "DrvEnableSurface(): AllocateNonOpaqueDeviceSurfaceOrBitmap() failed, status %d\n", Status));
        return NULL;
    }

    ppdev->EngSurfaceHandle = (HSURF) hsurf;
    ppdev->ScreenSurfaceDescriptor = (PVOID) pSurfaceDescriptor;

    pSurfaceDescriptor->IsScreen = TRUE;

    return hsurf;
}

/******************************Public*Routine******************************\
* DrvDisableSurface
*
* Free resources allocated by DrvEnableSurface. Release the surface.
*
\**************************************************************************/

VOID APIENTRY DrvDisableSurface(
    DHPDEV dhpdev
    )
{
    PDEV *ppdev = (PDEV *) dhpdev;

    DISPDBG((0, "DrvDisableSurface\n"));

    EngDeleteSurface(ppdev->EngSurfaceHandle);

    // deallocate SURFACE_DESCRIPTOR structure.

    FreeSurfaceDescriptor(ppdev->ScreenSurfaceDescriptor);
    ppdev->ScreenSurfaceDescriptor = NULL;
}

HBITMAP APIENTRY DrvCreateDeviceBitmap(
    IN DHPDEV dhpdev,
    IN SIZEL sizl,
    IN ULONG iFormat
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG ulBitCount;
    HSURF hsurf;
    LONG Status;

    PDEV *ppdev = (PDEV *) dhpdev;

    //DISPDBG((1,"CreateDeviceBitmap: %dx%d\n", sizl.cx, sizl.cy));

    switch (iFormat)
    {
    case BMF_8BPP:
        ulBitCount = 8;
        break;
    case BMF_16BPP:
        ulBitCount = 16;
        break;
    case BMF_24BPP:
        ulBitCount = 24;
        break;
    case BMF_32BPP:
        ulBitCount = 32;
        break;
    default:
        return NULL;
    }

    Status = AllocateNonOpaqueDeviceSurfaceOrBitmap(
        FALSE, ppdev->EngPdevHandle, ulBitCount, sizl, flGlobalHooks, &hsurf, &pSurfaceDescriptor, ppdev);

    if (Status < 0)
    {
        DISPDBG((0, "DrvCreateDeviceBitmap(): AllocateNonOpaqueDeviceSurfaceOrBitmap() failed, status %d\n", Status));
        return NULL;
    }

    pSurfaceDescriptor->IsScreen = FALSE;

    return (HBITMAP) hsurf;
}

VOID APIENTRY DrvDeleteDeviceBitmap(
    IN DHSURF dhsurf
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;

    // DISPDBG((1, "DeleteDeviceBitmap:\n"));

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) dhsurf;

    FreeSurfaceDescriptor(pSurfaceDescriptor);
}

BOOL APIENTRY DrvAssertMode(
    DHPDEV dhpdev,
    BOOL bEnable
    )
{
    PDEV *ppdev = (PDEV *) dhpdev;

    UNREFERENCED_PARAMETER(bEnable);
    UNREFERENCED_PARAMETER(ppdev);

    DISPDBG((0, "DrvAssertMode(%lx, %lx)\n", dhpdev, bEnable));

    return TRUE;
}

ULONG UserSupportVideoMode(
    ULONG cjIn,
    QV_SUPPORT_MODE *pQvSupportMode
    )
{
    if (cjIn < sizeof(QV_SUPPORT_MODE) || !pQvSupportMode)
        return QV_INVALID_PARAMETER;

    if (!IS_RESOLUTION_VALID(pQvSupportMode->Width, pQvSupportMode->Height))
        return QV_SUPPORT_MODE_INVALID_RESOLUTION;

    if (pQvSupportMode->Bpp != 16 && pQvSupportMode->Bpp != 24 && pQvSupportMode->Bpp != 32)
        return QV_SUPPORT_MODE_INVALID_BPP;

    DISPDBG((0, "SupportVideoMode(%ld, %ld, %d)\n", pQvSupportMode->Width, pQvSupportMode->Height, pQvSupportMode->Bpp));
    g_uWidth = pQvSupportMode->Width;
    g_uHeight = pQvSupportMode->Height;
    g_uBpp = pQvSupportMode->Bpp;

    return QV_SUCCESS;
}

ULONG UserGetSurfaceData(
    SURFOBJ *pso,
    ULONG cjIn,
    QV_GET_SURFACE_DATA *pQvGetSurfaceData,
    ULONG cjOut,
    QV_GET_SURFACE_DATA_RESPONSE *pQvGetSurfaceDataResponse
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    if (!pso || cjOut < sizeof(QV_GET_SURFACE_DATA_RESPONSE) || !pQvGetSurfaceDataResponse
        || cjIn < sizeof(QV_GET_SURFACE_DATA) || !pQvGetSurfaceData)
        return QV_INVALID_PARAMETER;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor || !pSurfaceDescriptor->Pdev)
    {
        // A surface is managed by GDI
        return QV_INVALID_PARAMETER;
    }

    pQvGetSurfaceDataResponse->Magic = QVIDEO_MAGIC;

    pQvGetSurfaceDataResponse->Width = pSurfaceDescriptor->Width;
    pQvGetSurfaceDataResponse->Height = pSurfaceDescriptor->Height;
    pQvGetSurfaceDataResponse->Delta = pSurfaceDescriptor->Delta;
    pQvGetSurfaceDataResponse->Bpp = pSurfaceDescriptor->BitCount;
    pQvGetSurfaceDataResponse->IsScreen = pSurfaceDescriptor->IsScreen;

    memcpy(pQvGetSurfaceData->PfnArray, pSurfaceDescriptor->pPfnArray,
        PFN_ARRAY_SIZE(pSurfaceDescriptor->Width, pSurfaceDescriptor->Height));

    return QV_SUCCESS;
}

BOOL CALLBACK UnmapDamageNotificationEventProc(
    DRIVEROBJ *pDriverObj
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pDriverObj->pvObj;
    DISPDBG((0, "UnmapDamageNotificationEventProc(): unmapping 0x%p\n", pSurfaceDescriptor->DamageNotificationEvent));

    EngUnmapEvent(pSurfaceDescriptor->DamageNotificationEvent);

    pSurfaceDescriptor->DamageNotificationEvent = NULL;
    pSurfaceDescriptor->DriverObj = NULL;

    return TRUE;
}

ULONG UserWatchSurface(
    SURFOBJ *pso,
    ULONG cjIn,
    QV_WATCH_SURFACE *pQvWatchSurface
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    PEVENT pDamageNotificationEvent = NULL;

    if (!pso || cjIn < sizeof(QV_WATCH_SURFACE) || !pQvWatchSurface)
        return QV_INVALID_PARAMETER;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
    {
        // A surface is managed by GDI
        return QV_INVALID_PARAMETER;
    }

    DISPDBG((0, "WatchSurface(%p): hEvent 0x%x\n", pso, pQvWatchSurface->UserModeEvent));

    if (pSurfaceDescriptor->DriverObj)
    {
        DISPDBG((0, "WatchSurface(%p): Surface is already watched\n", pso));
        return QV_INVALID_PARAMETER;
    }

    pDamageNotificationEvent = EngMapEvent(pso->hdev, pQvWatchSurface->UserModeEvent, NULL, NULL, NULL);
    if (!pDamageNotificationEvent)
    {
        DISPDBG((0, "WatchSurface(): EngMapEvent(0x%x) failed, error %d\n", pQvWatchSurface->UserModeEvent, EngGetLastError()));
        return QV_INVALID_HANDLE;
    }

    DISPDBG((0, "WatchSurface(): pEvent 0x%p\n", pDamageNotificationEvent));

    // *Maybe* this surface can be accessed by UnmapDamageNotificationEventProc() or DrvSynchronizeSurface() right after EngCreateDriverObj(),
    // so pSurfaceDescriptor->pDamageNotificationEvent must be set before calling it.
    pSurfaceDescriptor->DamageNotificationEvent = pDamageNotificationEvent;

    // Install a notification callback for process deletion.
    // We have to unmap the event if the client process terminates unexpectedly.
    pSurfaceDescriptor->DriverObj = EngCreateDriverObj(pSurfaceDescriptor, UnmapDamageNotificationEventProc, pso->hdev);
    if (!pSurfaceDescriptor->DriverObj)
    {
        DISPDBG((0, "WatchSurface(): EngCreateDriverObj(0x%p) failed, error %d\n", pSurfaceDescriptor, EngGetLastError()));

        pSurfaceDescriptor->DamageNotificationEvent = NULL;
        EngUnmapEvent(pDamageNotificationEvent);
        return QV_INVALID_HANDLE;
    }

    DISPDBG((0, "WatchSurface(): hDriverObj 0x%p\n", pSurfaceDescriptor->DriverObj));

    return QV_SUCCESS;
}

ULONG UserStopWatchingSurface(
    SURFOBJ *pso
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;

    if (!pso)
        return QV_INVALID_PARAMETER;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
    {
        // A surface is managed by GDI
        return QV_INVALID_PARAMETER;
    }

    if (!pSurfaceDescriptor->DriverObj)
    {
        DISPDBG((0, "StopWatchingSurface(): hDriverObj is zero\n"));
        return QV_INVALID_PARAMETER;
    }

    DISPDBG((0, "StopWatchingSurface(%p)\n", pso));

    // Require a cleanup callback to be called.
    // pSurfaceDescriptor->hDriverObj will be set to NULL by the callback.
    EngDeleteDriverObj(pSurfaceDescriptor->DriverObj, TRUE, FALSE);

    return QV_SUCCESS;
}

ULONG APIENTRY DrvEscape(
    SURFOBJ *pso,
    ULONG iEsc,
    ULONG cjIn,
    void *pvIn,
    ULONG cjOut,
    void *pvOut
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor;

    DISPDBG((0, "DrvEscape: pso=%p, code=%x\n", pso, iEsc));
    if ((cjIn < sizeof(ULONG)) || !pvIn || (*(PULONG) pvIn != QVIDEO_MAGIC))
    {
        // 0 means "not supported"
        DISPDBG((0, "DrvEscape: bad size/magic\n"));
        return 0;
    }

    // FIXME: validate user buffers!

    switch (iEsc)
    {
    case QVESC_SUPPORT_MODE:
        return UserSupportVideoMode(cjIn, pvIn);

    case QVESC_GET_SURFACE_DATA:
        return UserGetSurfaceData(pso, cjIn, pvIn, cjOut, pvOut);

    case QVESC_WATCH_SURFACE:
        return UserWatchSurface(pso, cjIn, pvIn);

    case QVESC_STOP_WATCHING_SURFACE:
        return UserStopWatchingSurface(pso);

    case QVESC_SYNCHRONIZE:
        if (!g_bUseDirtyBits)
            return 0;
        
        pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
        if (!pSurfaceDescriptor || !pSurfaceDescriptor->Pdev)
            return QV_INVALID_PARAMETER;

        InterlockedExchange(&pSurfaceDescriptor->DirtyPages->Ready, 1);
        DISPDBG((0, "WGA synchronized\n"));
        return QV_SUCCESS;

    default:
        DISPDBG((0, "DrvEscape: bad code %x\n", iEsc));
        // 0 means "not supported"
        return 0;
    }
}

VOID APIENTRY DrvSynchronizeSurface(
    SURFOBJ *pso,
    RECTL *prcl,
    FLONG fl
    )
{
    SURFACE_DESCRIPTOR *pSurfaceDescriptor = NULL;
    ULONG uSize, uDirty;

    UNREFERENCED_PARAMETER(prcl);
    UNREFERENCED_PARAMETER(fl);

    if (!pso)
        return;

    pSurfaceDescriptor = (SURFACE_DESCRIPTOR *) pso->dhsurf;
    if (!pSurfaceDescriptor)
        return;

    if (!pSurfaceDescriptor->DamageNotificationEvent)
        // This surface is not watched.
        return;

    // surface buffer size
    uSize = pSurfaceDescriptor->Delta * pSurfaceDescriptor->Height;

    // UpdateDirtyBits returns 0 also if the check was too early after a previous one.
    // This just returns 1 if using dirty bits is disabled.
    uDirty = UpdateDirtyBits(pSurfaceDescriptor->SurfaceData, uSize, pSurfaceDescriptor->DirtyPages);

    if (uDirty > 0) // only signal the event if something changed
        EngSetEvent(pSurfaceDescriptor->DamageNotificationEvent);
}
