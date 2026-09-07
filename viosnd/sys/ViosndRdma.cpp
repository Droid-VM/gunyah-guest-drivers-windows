// SPDX-License-Identifier: BSD-3-Clause
#include "precomp.h"
#include "ViosndRdma.h"

#define VIOSND_RDMA_TAG 'aRSV'

static __forceinline ULONG ViosndRdmaPagesFor(_In_ SIZE_T Size)
{
    return (ULONG)((Size + PAGE_SIZE - 1) / PAGE_SIZE);
}

NTSTATUS ViosndRdmaOpen(_Inout_ PVIOSND_RDMA Rdma)
{
    NTSTATUS status;

    RtlZeroMemory(Rdma, sizeof(*Rdma));
    KeInitializeSpinLock(&Rdma->Lock);

    status = RdmaClientOpen(&Rdma->Client, "viosnd");
    if (!NT_SUCCESS(status))
    {
        return status;
    }

    Rdma->Opened = TRUE;
    DbgPrint("viosnd rdmapool: open, pool=%I64u bytes\n", Rdma->Client.LastPoolTotalSize);
    return STATUS_SUCCESS;
}

/*
 * Takes one more region of at least Pages pages and publishes it.
 *
 * Must run at PASSIVE_LEVEL and WITHOUT the lock held: reaching the pool means an IOCTL, and
 * RdmaClientIoctl waits on an event for the completion. Doing that inside the spin lock -- which
 * is what the first version of this did -- raises IRQL to DISPATCH and then blocks on it, so
 * device start simply stopped, with the device reported as started and nothing else to see.
 */
static PVIOSND_RDMA_REGION ViosndRdmaAddRegion(_Inout_ PVIOSND_RDMA Rdma, _In_ ULONG Pages)
{
    PVIOSND_RDMA_REGION region;
    PVOID va = NULL;
    PHYSICAL_ADDRESS pa;
    SIZE_T bitmapBytes;
    PULONG bitmapBuffer;
    ULONG want;
    NTSTATUS status;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
    {
        /* The pool cannot be reached from here. Every allocation this driver makes is on the
         * device-start or stream-start path, both at PASSIVE_LEVEL; a future caller that is not
         * gets a failure rather than a blocked machine. */
        DbgPrint("viosnd rdmapool: cannot take a region at IRQL %u\n", KeGetCurrentIrql());
        return NULL;
    }
    if (Rdma->RegionCount >= VIOSND_RDMA_MAX_REGIONS)
    {
        DbgPrint("viosnd rdmapool: region table full (%u)\n", VIOSND_RDMA_MAX_REGIONS);
        return NULL;
    }

    /* Round up so a run of small allocations shares one region rather than
     * scattering the pool, but never below what was actually asked for. */
    want = Pages;
    if (want < VIOSND_RDMA_REGION_GRAIN_PAGES)
    {
        want = VIOSND_RDMA_REGION_GRAIN_PAGES;
    }

    /*
     * Ask for the rounded-up size, and settle for the exact size if the pool
     * cannot spare it. Both are worth trying: the first keeps the pool tidy,
     * the second is the difference between a working device and none.
     */
    status = RdmaClientAllocRegion(&Rdma->Client, want, &va, &pa);
    if (!NT_SUCCESS(status) && want != Pages)
    {
        DbgPrint("viosnd rdmapool: %u pages refused (0x%x), asking for %u\n", want, status, Pages);
        want = Pages;
        status = RdmaClientAllocRegion(&Rdma->Client, want, &va, &pa);
    }
    if (!NT_SUCCESS(status))
    {
        DbgPrint("viosnd rdmapool: %u pages refused (0x%x)\n", want, status);
        return NULL;
    }

    /* RtlInitializeBitMap wants a ULONG-aligned buffer sized in whole ULONGs. */
    bitmapBytes = ((SIZE_T)((want + 31) / 32)) * sizeof(ULONG);
    bitmapBuffer = (PULONG)ExAllocatePoolUninitialized(NonPagedPoolNx, bitmapBytes, VIOSND_RDMA_TAG);
    if (bitmapBuffer == NULL)
    {
        RdmaClientFreeRegion(&Rdma->Client, va, want);
        return NULL;
    }

    /* Publish it under the lock; everything above was done without one. */
    {
        KIRQL irql;

        KeAcquireSpinLock(&Rdma->Lock, &irql);
        if (Rdma->RegionCount >= VIOSND_RDMA_MAX_REGIONS)
        {
            /* Another thread filled the table while this one was in the IOCTL. */
            KeReleaseSpinLock(&Rdma->Lock, irql);
            ExFreePoolWithTag(bitmapBuffer, VIOSND_RDMA_TAG);
            RdmaClientFreeRegion(&Rdma->Client, va, want);
            return NULL;
        }
        region = &Rdma->Regions[Rdma->RegionCount];
        region->BaseVA = va;
        region->BasePA = pa;
        region->Pages = want;
        region->BitmapBuffer = bitmapBuffer;
        RtlZeroMemory(bitmapBuffer, bitmapBytes);
        RtlInitializeBitMap(&region->Bitmap, bitmapBuffer, want);
        RtlClearAllBits(&region->Bitmap);
        Rdma->RegionCount++;
        KeReleaseSpinLock(&Rdma->Lock, irql);
    }

    DbgPrint("viosnd rdmapool: region %u = %u pages at VA=%p PA=0x%I64x\n",
             Rdma->RegionCount - 1,
             want,
             va,
             pa.QuadPart);
    return region;
}

VOID ViosndRdmaClose(_Inout_ PVIOSND_RDMA Rdma)
{
    for (ULONG i = 0; i < Rdma->RegionCount; ++i)
    {
        PVIOSND_RDMA_REGION region = &Rdma->Regions[i];
        if (region->BitmapBuffer != NULL)
        {
            ExFreePoolWithTag(region->BitmapBuffer, VIOSND_RDMA_TAG);
            region->BitmapBuffer = NULL;
        }
        if (region->BaseVA != NULL)
        {
            RdmaClientFreeRegion(&Rdma->Client, region->BaseVA, region->Pages);
            region->BaseVA = NULL;
        }
        region->Pages = 0;
    }
    Rdma->RegionCount = 0;
    Rdma->Opened = FALSE;
    RdmaClientClose(&Rdma->Client);
}

_Ret_maybenull_ PVOID ViosndRdmaAlloc(_Inout_ PVIOSND_RDMA Rdma,
                                      _In_ SIZE_T Size,
                                      _Out_ PPHYSICAL_ADDRESS LogicalAddress)
{
    KIRQL irql;
    ULONG pages;
    PVOID va = NULL;

    LogicalAddress->QuadPart = 0;
    if (!ViosndRdmaActive(Rdma))
    {
        return NULL;
    }

    pages = ViosndRdmaPagesFor(Size);
    if (pages == 0)
    {
        return NULL;
    }

    /*
     * Two passes: satisfy from what is already held, and if nothing has a long enough run, take
     * one more region and look again. The region is taken with the lock released -- reaching the
     * pool blocks on an IOCTL -- so the second pass re-examines every region rather than only
     * the new one: another thread may have added or freed space in between.
     */
    for (ULONG attempt = 0; attempt < 2 && va == NULL; ++attempt)
    {
        KeAcquireSpinLock(&Rdma->Lock, &irql);
        for (ULONG i = 0; i < Rdma->RegionCount; ++i)
        {
            PVIOSND_RDMA_REGION region = &Rdma->Regions[i];
            ULONG index;

            if (pages > region->Pages)
            {
                continue;
            }
            index = RtlFindClearBitsAndSet(&region->Bitmap, pages, 0);
            if (index == 0xFFFFFFFF)
            {
                continue;
            }
            va = (PVOID)((PUCHAR)region->BaseVA + ((SIZE_T)index * PAGE_SIZE));
            LogicalAddress->QuadPart = region->BasePA.QuadPart + ((LONGLONG)index * PAGE_SIZE);
            break;
        }
        KeReleaseSpinLock(&Rdma->Lock, irql);

        if (va == NULL && attempt == 0 && ViosndRdmaAddRegion(Rdma, pages) == NULL)
        {
            break;
        }
    }

    if (va == NULL)
    {
        DbgPrint("viosnd rdmapool: no room for %u pages across %u region(s)\n", pages, Rdma->RegionCount);
        return NULL;
    }

    /* The device reads whatever is here; never hand it the previous stream's
     * audio. */
    RtlZeroMemory(va, (SIZE_T)pages * PAGE_SIZE);
    return va;
}

VOID ViosndRdmaFree(_Inout_ PVIOSND_RDMA Rdma, _In_opt_ PVOID Va, _In_ SIZE_T Size)
{
    KIRQL irql;
    ULONG pages;

    if (Va == NULL || !ViosndRdmaActive(Rdma))
    {
        return;
    }
    pages = ViosndRdmaPagesFor(Size);
    if (pages == 0)
    {
        return;
    }

    ULONG foundRegion = VIOSND_RDMA_MAX_REGIONS;
    ULONG foundStart = 0;
    ULONG foundPages = 0;
    BOOLEAN cleared = FALSE;

    KeAcquireSpinLock(&Rdma->Lock, &irql);
    for (ULONG i = 0; i < Rdma->RegionCount; ++i)
    {
        PVIOSND_RDMA_REGION region = &Rdma->Regions[i];
        PUCHAR base = (PUCHAR)region->BaseVA;
        SIZE_T span = (SIZE_T)region->Pages * PAGE_SIZE;
        ULONG start;

        if ((PUCHAR)Va < base || (PUCHAR)Va >= base + span)
        {
            continue;
        }
        start = (ULONG)(((PUCHAR)Va - base) / PAGE_SIZE);
        foundRegion = i;
        foundStart = start;
        foundPages = region->Pages;
        /* RtlClearBits does not range-check in a free build, so a Size larger than what was
         * allocated here would clear bits past the end of the bitmap buffer -- corrupting the
         * pool heap rather than the audio. Every caller pairs the block's own rounded size with
         * its own pointer, so this cannot fire today; when it does, the allocation it belongs to
         * is what is wrong, and losing the pages is the cheaper half of that. */
        if (pages <= region->Pages - start)
        {
            RtlClearBits(&region->Bitmap, start, pages);
            cleared = TRUE;
        }
        break;
    }
    KeReleaseSpinLock(&Rdma->Lock, irql);

    if (!cleared)
    {
        /* Silence here means pages that never come back and no way to tell from the outside. */
        if (foundRegion == VIOSND_RDMA_MAX_REGIONS)
        {
            DbgPrint("viosnd rdmapool: free of %p (%u pages) matches no region; leaked\n", Va, pages);
        }
        else
        {
            DbgPrint("viosnd rdmapool: free of %u pages at +%u overruns region %u (%u pages); leaked\n",
                     pages,
                     foundStart,
                     foundRegion,
                     foundPages);
        }
    }
    /* Regions are kept once taken: a device that just freed a buffer is very
     * likely about to want one the same size, and handing the pages back only
     * to ask for them again invites another client to take the gap. They go
     * back at ViosndRdmaClose. */
}

ULONG ViosndRdmaHeldPages(_In_ PVIOSND_RDMA Rdma)
{
    ULONG total = 0;

    for (ULONG i = 0; i < Rdma->RegionCount; ++i)
    {
        total += Rdma->Regions[i].Pages;
    }
    return total;
}
