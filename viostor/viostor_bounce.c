/*
 * Bounce buffer allocator for restricted DMA pool (rdmapool) support.
 *
 * Uses lock-free SLIST for both control slots and data pages,
 * making it safe to call at any IRQL including DIRQL.
 */

#include "virtio_stor.h"
#include "viostor_bounce.h"

#if defined(EVENT_TRACING)
#include "viostor_bounce.tmh"
#endif

NTSTATUS
BounceInit(PBOUNCE_ALLOCATOR Alloc, PUCHAR BaseVA, PHYSICAL_ADDRESS BasePA, SIZE_T TotalSize, ULONG CtlSlotCount)
{
    ULONG ctlRegionSize;
    ULONG totalPages;
    ULONG dataPages;
    ULONG i;
    PUCHAR ptr;

    RtlZeroMemory(Alloc, sizeof(*Alloc));

    totalPages = (ULONG)(TotalSize / PAGE_SIZE);
    ctlRegionSize = CtlSlotCount * BOUNCE_CTL_PAGES;

    if (totalPages <= ctlRegionSize)
    {
        RhelDbgPrint(TRACE_LEVEL_ERROR,
                     " BounceInit: insufficient pages (%u) for %u control slots\n",
                     totalPages,
                     CtlSlotCount);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    dataPages = totalPages - ctlRegionSize;

    Alloc->BaseVA = BaseVA;
    Alloc->BasePA = BasePA;
    Alloc->TotalPages = totalPages;

    /* Control slot region starts at BaseVA */
    Alloc->CtlBaseVA = BaseVA;
    Alloc->CtlSlotCount = CtlSlotCount;
    InitializeSListHead(&Alloc->CtlFreeList);

    for (i = 0; i < CtlSlotCount; i++)
    {
        ptr = BaseVA + (SIZE_T)i * BOUNCE_CTL_SIZE;
        RtlZeroMemory(ptr, BOUNCE_CTL_SIZE);
        InterlockedPushEntrySList(&Alloc->CtlFreeList, (PSLIST_ENTRY)ptr);
    }

    /* Data page region starts after control slots */
    Alloc->DataBaseVA = BaseVA + (SIZE_T)ctlRegionSize * PAGE_SIZE;
    Alloc->DataPageCount = dataPages;
    InitializeSListHead(&Alloc->DataFreeList);

    for (i = 0; i < dataPages; i++)
    {
        ptr = Alloc->DataBaseVA + (SIZE_T)i * PAGE_SIZE;
        InterlockedPushEntrySList(&Alloc->DataFreeList, (PSLIST_ENTRY)ptr);
    }

    Alloc->Initialized = TRUE;

    RhelDbgPrint(TRACE_LEVEL_INFORMATION,
                 " BounceInit: %u ctl slots (%u pages), %u data pages, total %u pages\n",
                 CtlSlotCount,
                 ctlRegionSize,
                 dataPages,
                 totalPages);

    return STATUS_SUCCESS;
}

PVOID
BounceAllocCtl(PBOUNCE_ALLOCATOR Alloc)
{
    return (PVOID)InterlockedPopEntrySList(&Alloc->CtlFreeList);
}

VOID BounceFreeCtl(PBOUNCE_ALLOCATOR Alloc, PVOID CtlVA)
{
    InterlockedPushEntrySList(&Alloc->CtlFreeList, (PSLIST_ENTRY)CtlVA);
}

PVOID
BounceAllocDataPage(PBOUNCE_ALLOCATOR Alloc)
{
    return (PVOID)InterlockedPopEntrySList(&Alloc->DataFreeList);
}

VOID BounceFreeDataPage(PBOUNCE_ALLOCATOR Alloc, PVOID PageVA)
{
    InterlockedPushEntrySList(&Alloc->DataFreeList, (PSLIST_ENTRY)PageVA);
}
